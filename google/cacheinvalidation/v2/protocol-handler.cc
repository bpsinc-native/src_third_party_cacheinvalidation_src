// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Client for interacting with low-level protocol messages.

#include "google/cacheinvalidation/v2/protocol-handler.h"

#include "google/cacheinvalidation/v2/string_util.h"
#include "google/cacheinvalidation/v2/constants.h"
#include "google/cacheinvalidation/v2/log-macro.h"
#include "google/cacheinvalidation/v2/proto-helpers.h"

namespace invalidation {

using ::ipc::invalidation::ConfigChangeMessage;
using ::ipc::invalidation::InfoMessage;
using ::ipc::invalidation::InitializeMessage;
using ::ipc::invalidation::InitializeMessage_DigestSerializationType_BYTE_BASED;
using ::ipc::invalidation::InvalidationMessage;
using ::ipc::invalidation::PropertyRecord;
using ::ipc::invalidation::RegistrationMessage;
using ::ipc::invalidation::RegistrationSyncMessage;
using ::ipc::invalidation::ServerHeader;
using ::ipc::invalidation::ServerToClientMessage;
using ::ipc::invalidation::TokenControlMessage;

ProtocolHandler::ProtocolHandler(
    const Config& config, SystemResources* resources, Statistics* statistics,
    const string& application_name, ProtocolListener* listener,
    TiclMessageValidator* msg_validator)
    : resources_(resources),
      logger_(resources->logger()),
      internal_scheduler_(resources->internal_scheduler()),
      listener_(listener),
      operation_scheduler_(new OperationScheduler(
          logger_, internal_scheduler_)),
      msg_validator_(msg_validator),
      message_id_(1),
      last_known_server_time_ms_(0),
      next_message_send_time_ms_(0),
      statistics_(statistics),
      batching_task_(NewPermanentCallback(
          this, &ProtocolHandler::BatchingTask)) {
  // Initialize client version.
  client_version_.mutable_version()->set_major_version(
      Constants::kClientMajorVersion);
  client_version_.mutable_version()->set_minor_version(
      Constants::kClientMinorVersion);
  client_version_.set_platform(resources->platform());
  client_version_.set_language("C++");
  client_version_.set_application_info(application_name);

  operation_scheduler_->SetOperation(
      config.batching_delay, batching_task_.get());

  // Install ourselves as a receiver for server messages.
  resources_->network()->SetMessageReceiver(
      NewPermanentCallback(this, &ProtocolHandler::MessageReceiver));

  resources_->network()->AddNetworkStatusReceiver(
      NewPermanentCallback(this, &ProtocolHandler::NetworkStatusReceiver));
}

void ProtocolHandler::HandleIncomingMessage(string incoming_message) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  ServerToClientMessage message;
  message.ParseFromString(incoming_message);
  if (!message.IsInitialized()) {
    TLOG(logger_, WARNING, "Incoming message is unparseable: %s",
         incoming_message.c_str());
    return;
  }

  // Validate the message. If this passes, we can blindly assume valid messages
  // from here on.
  TLOG(logger_, FINE, "Incoming message: %s",
       ProtoHelpers::ToString(message).c_str());

  if (!msg_validator_->IsValid(message)) {
    statistics_->RecordError(
        Statistics::ClientErrorType_INCOMING_MESSAGE_FAILURE);
    TLOG(logger_, SEVERE, "Received invalid message: %s",
         ProtoHelpers::ToString(message).c_str());
    return;
  }

  statistics_->RecordReceivedMessage(Statistics::ReceivedMessageType_TOTAL);

  // Construct a representation of the message header.
  const ServerHeader& message_header = message.header();
  ServerMessageHeader header(
      message_header.client_token(),
      message_header.registration_summary());

  // Check the version of the message
  if (message_header.protocol_version().version().major_version() !=
      Constants::kProtocolMajorVersion) {
    statistics_->RecordError(
        Statistics::ClientErrorType_PROTOCOL_VERSION_FAILURE);
    TLOG(logger_, SEVERE, "Dropping message with incompatible version: %s",
         ProtoHelpers::ToString(message).c_str());
    return;
  }

  // Check if it is a ConfigChangeMessage which indicates that messages should
  // no longer be sent for a certain duration. Perform this check before the
  // token is even checked.
  if (message.has_config_change_message()) {
    const ConfigChangeMessage& config_change_msg =
        message.config_change_message();
    if (config_change_msg.has_next_message_delay_ms()) {
      // Validator has ensured that it is positive.
      next_message_send_time_ms_ = GetCurrentTimeMs() +
          config_change_msg.next_message_delay_ms();
    }
    return;  // Ignore all other messages in the envelope.
  }

  // Check token if possible.
  if (!CheckServerToken(message_header.client_token())) {
    return;
  }

  if (message_header.server_time_ms() > last_known_server_time_ms_) {
    last_known_server_time_ms_ = message_header.server_time_ms();
  }

  // Invoke callbacks as appropriate.
  if (message.has_token_control_message()) {
    const TokenControlMessage& token_msg = message.token_control_message();
    statistics_->RecordReceivedMessage(
        Statistics::ReceivedMessageType_TOKEN_CONTROL);
    listener_->HandleTokenChanged(
        header, token_msg.new_token(), token_msg.status());
  }

  // We explicitly check to see if we have a valid token after we pass the token
  // control message to the listener. This is because we can't determine whether
  // we have a valid token until after the upcall:
  // 1) The listener might have acquired a token.
  // 2) The listener might have lost its token.
  // Note that checking for the presence of a TokenControlMessage is *not*
  // sufficient: it might be a token-assign with the wrong nonce or a
  // token-destroy message, for example.
  if (listener_->GetClientToken().empty()) {
    return;
  }
  if (message.has_invalidation_message()) {
    statistics_->RecordReceivedMessage(
        Statistics::ReceivedMessageType_INVALIDATION);
    listener_->HandleInvalidations(
        header, message.invalidation_message().invalidation());
  }
  if (message.has_registration_status_message()) {
    statistics_->RecordReceivedMessage(
        Statistics::ReceivedMessageType_REGISTRATION_STATUS);
    listener_->HandleRegistrationStatus(
        header, message.registration_status_message().registration_status());
  }
  if (message.has_registration_sync_request_message()) {
    statistics_->RecordReceivedMessage(
        Statistics::ReceivedMessageType_REGISTRATION_SYNC_REQUEST);
    listener_->HandleRegistrationSyncRequest(header);
  }
  if (message.has_info_request_message()) {
    statistics_->RecordReceivedMessage(
        Statistics::ReceivedMessageType_INFO_REQUEST);
    listener_->HandleInfoMessage(
        header, message.info_request_message().info_type());
  }
}

bool ProtocolHandler::CheckServerToken(const string& server_token) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  const string& client_token = listener_->GetClientToken();

  // If we do not have a client token yet, there is nothing to compare. The
  // message must have an initialize message and the upper layer will do the
  // appropriate checks. Hence, we return true for clientToken == null.
  if (client_token.empty()) {
    // No token. Return true so that we'll attempt to deliver a token control
    // message (if any) to the listener in handleIncomingMessage.
    return true;
  }

  if (client_token != server_token) {
    // Bad token - reject whole message
    TLOG(logger_, WARNING, "Incoming message has bad token: %s, %s",
         client_token.c_str(), server_token.c_str());
    statistics_->RecordError(Statistics::ClientErrorType_TOKEN_MISMATCH);
    return false;
  }
  return true;
}

void ProtocolHandler::SendInitializeMessage(
    int client_type, const ApplicationClientIdP& application_client_id,
    const string& nonce, const string& debug_string) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";

  InitializeMessage init_msg;
  init_msg.set_client_type(client_type);
  init_msg.mutable_application_client_id()->CopyFrom(application_client_id);
  init_msg.set_nonce(nonce);
  init_msg.set_digest_serialization_type(
      InitializeMessage_DigestSerializationType_BYTE_BASED);
  statistics_->RecordSentMessage(Statistics::SentMessageType_INITIALIZE);

  ClientToServerMessage message;
  message.mutable_initialize_message()->CopyFrom(init_msg);
  SendMessageToServer(&message, "Init-" + debug_string);
}

void ProtocolHandler::SendInfoMessage(
    const vector<pair<string, int> >& performance_counters,
    const vector<pair<string, int> >& config_params) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  InfoMessage info_message;
  info_message.mutable_client_version()->CopyFrom(client_version_);

  // Add configuration parameters.
  for (size_t i = 0; i < config_params.size(); ++i) {
    PropertyRecord* config_record = info_message.add_config_paramter();
    config_record->set_name(config_params[i].first);
    config_record->set_value(config_params[i].second);
  }

  // Add performance counters.
  for (size_t i = 0; i < performance_counters.size(); ++i) {
    PropertyRecord* counter = info_message.add_performance_counter();
    counter->set_name(performance_counters[i].first);
    counter->set_value(performance_counters[i].second);
  }
  statistics_->RecordSentMessage(Statistics::SentMessageType_INFO);
  ClientToServerMessage message;
  message.mutable_info_message()->CopyFrom(info_message);
  SendMessageToServer(&message, "Info");
}

void ProtocolHandler::SendRegistrations(
    const vector<ObjectIdP>& object_ids, RegistrationP::OpType reg_op_type) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  for (size_t i = 0; i < object_ids.size(); ++i) {
    pending_registrations_[object_ids[i]] = reg_op_type;
  }
  operation_scheduler_->Schedule(batching_task_.get());
}

void ProtocolHandler::SendInvalidationAck(const InvalidationP& invalidation) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  // We could do squelching - we don't since it is unlikely to be too beneficial
  // here.
  acked_invalidations_.insert(invalidation);
  operation_scheduler_->Schedule(batching_task_.get());
}

void ProtocolHandler::SendRegistrationSyncSubtree(
    const RegistrationSubtree& reg_subtree) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  registration_subtrees_.insert(reg_subtree);
  TLOG(logger_, INFO, "Adding subtree: %s",
       ProtoHelpers::ToString(reg_subtree).c_str());
  operation_scheduler_->Schedule(batching_task_.get());
}

void ProtocolHandler::SendMessageToServer(
    ClientToServerMessage* builder, const string& debug_string) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";

  if (next_message_send_time_ms_ > GetCurrentTimeMs()) {
    TLOG(logger_, WARNING, "In quiet period: not sending message to server: "
         "%lld > %lld", next_message_send_time_ms_, GetCurrentTimeMs());
    return;
  }

  // Note: Even if an initialize message is being sent, we can send additional
  // messages such as regisration messages, etc to the server. But if there is
  // no token and an initialize message is not being sent, we cannot send any
  // other message.

  if ((listener_->GetClientToken().empty()) &&
      !builder->has_initialize_message()) {
    // Cannot send any message
    TLOG(logger_, WARNING,
         "Cannot send message since no token and no initialze msg: %s, %s",
         debug_string.c_str(), ProtoHelpers::ToString(*builder).c_str());
    statistics_->RecordError(Statistics::ClientErrorType_TOKEN_MISSING_FAILURE);
    return;
  }

  ClientHeader* outgoing_header = builder->mutable_header();
  InitClientHeader(outgoing_header);

  // Check for pending batched operations and add to message builder if needed.

  // Add reg, acks, reg subtrees - clear them after adding.
  if (!acked_invalidations_.empty()) {
    InvalidationMessage* ack_message =
        builder->mutable_invalidation_ack_message();
    hash_set<InvalidationP, ProtoHash, ProtoEq>::const_iterator iter;
    for (iter = acked_invalidations_.begin();
         iter != acked_invalidations_.end(); ++iter) {
      ack_message->add_invalidation()->CopyFrom(*iter);
    }
    acked_invalidations_.clear();
    statistics_->RecordSentMessage(
        Statistics::SentMessageType_INVALIDATION_ACK);
  }

  // Check regs.
  if (!pending_registrations_.empty()) {
    hash_map<ObjectIdP, RegistrationP::OpType, ProtoHash,
        ProtoEq>::const_iterator iter;
    RegistrationMessage* reg_message = builder->mutable_registration_message();
    for (iter = pending_registrations_.begin();
         iter != pending_registrations_.end(); ++iter) {
      RegistrationP* reg = reg_message->add_registration();
      reg->mutable_object_id()->CopyFrom(iter->first);
      reg->set_op_type(iter->second);
    }
    pending_registrations_.clear();
    statistics_->RecordSentMessage(Statistics::SentMessageType_REGISTRATION);
  }

  // Check reg substrees.
  if (!registration_subtrees_.empty()) {
    RegistrationSyncMessage* sync_message =
        builder->mutable_registration_sync_message();
    hash_set<RegistrationSubtree, ProtoHash, ProtoEq>::const_iterator iter;
    for (iter = registration_subtrees_.begin();
         iter != registration_subtrees_.end(); ++iter) {
      sync_message->add_subtree()->CopyFrom(*iter);
    }
    registration_subtrees_.clear();
    statistics_->RecordSentMessage(
        Statistics::SentMessageType_REGISTRATION_SYNC);
  }

  // Validate the message and send it.
  ++message_id_;
  if (!msg_validator_->IsValid(*builder)) {
    TLOG(logger_, SEVERE, "(%s): Tried to send invalid message: %s",
         debug_string.c_str(), ProtoHelpers::ToString(*builder).c_str());
    statistics_->RecordError(
        Statistics::ClientErrorType_OUTGOING_MESSAGE_FAILURE);
    return;
  }

  TLOG(logger_, FINE, "(%s) Sending message to server: %s",
       debug_string.c_str(), ProtoHelpers::ToString(*builder).c_str());
  statistics_->RecordSentMessage(Statistics::SentMessageType_TOTAL);
  string serialized;
  builder->SerializeToString(&serialized);
  resources_->network()->SendMessage(serialized);
}

void ProtocolHandler::InitClientHeader(ClientHeader* builder) {
  CHECK(internal_scheduler_->IsRunningOnThread()) << "Not on internal thread";
  builder->mutable_protocol_version()->mutable_version()->set_major_version(
      Constants::kProtocolMajorVersion);
  builder->mutable_protocol_version()->mutable_version()->set_minor_version(
      Constants::kProtocolMinorVersion);
  builder->set_client_time_ms(GetCurrentTimeMs());
  builder->set_message_id(StringPrintf("%d", message_id_++));
  builder->set_max_known_server_time_ms(last_known_server_time_ms_);
  listener_->GetRegistrationSummary(builder->mutable_registration_summary());
  const string& client_token = listener_->GetClientToken();
  if (client_token != "") {
    TLOG(logger_, FINE, "Sending token on client->server message: %s",
         client_token.c_str());
    builder->set_client_token(client_token);
  }
}

void ProtocolHandler::BatchingTask() {
  ClientToServerMessage message;
  SendMessageToServer(&message, "BatchingTask");
}

void ProtocolHandler::MessageReceiver(string message) {
  internal_scheduler_->Schedule(Scheduler::kNoDelay, NewPermanentCallback(this,
      &ProtocolHandler::HandleIncomingMessage, message));
}

void ProtocolHandler::NetworkStatusReceiver(bool status) {
  // Do nothing for now.
}

}  // namespace invalidation
