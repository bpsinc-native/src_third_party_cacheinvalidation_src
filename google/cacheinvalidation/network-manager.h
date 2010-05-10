// Copyright 2010 Google Inc.
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

#ifndef GOOGLE_CACHEINVALIDATION_NETWORK_MANAGER_H_
#define GOOGLE_CACHEINVALIDATION_NETWORK_MANAGER_H_

#include "google/cacheinvalidation/callback.h"
#include "google/cacheinvalidation/invalidation-client.h"
#include "google/cacheinvalidation/throttle.h"
#include "google/cacheinvalidation/time.h"
#include "google/cacheinvalidation/types.pb.h"

namespace invalidation {

class ClientConfig;
class InvalidationClientImpl;
class NetworkEndpoint;
class SystemResources;

/* Keeps track of whether there is outbound data to be sent and informs the
 * application when appropriate.  Handles heartbeats and polling for
 * invalidations, and keeps the intervals for these up-to-date in response to
 * messages from the server.
 *
 * This is an internal helper class for InvalidationClientImpl.
 */
class NetworkManager {
 private:
  /* Constructs a network manager with the given endpoint, resources, and
   * configuration parameters.
   */
  NetworkManager(NetworkEndpoint* endpoint, SystemResources* resources,
                 const ClientConfig& config);

  /* If have_session is true and it has been at least poll_delay since we
   * last polled for invalidations, sets a POLL_INVALIDATIONS action on the
   * message.
   *
   * message - a message being prepared for sending to the server
   *
   * is_object_control - whether the outbound message is of type OBJECT_CONTROL
   */
  void HandleOutboundMessage(
      ClientToServerMessage* message, bool is_object_control);

  /* Updates the heartbeat and polling intervals if these are present in the
   * bundle.
   */
  void HandleInboundMessage(const ServerToClientMessage& bundle);

  /* Returns whether a heartbeat task should be performed, i.e., whether enough
   * time has elapsed since the last communication with the server.
   */
  bool HeartbeatNeeded() {
    // If there's been no network traffic for the last heartbeatDelay_ ms, then
    // send a heartbeat.
    return resources_->current_time() >= last_send_ + heartbeat_delay_;
  }

  /* Indicates that the Ticl has data it's ready to send to the server.  If a
   * network listener has been registered and it hasn't been informed about
   * outbound data since it last pulled a message, let it know.
   */
  void OutboundDataReady();

  /* Registers a listener to be notified when outbound data becomes available.
   * If there is outbound data already waiting to be send, notifies it
   * immediately.
   */
  void RegisterOutboundListener(
      NetworkCallback* outbound_message_ready);

  /* Calls DoInformOutboundListener() through a throttler. */
  void InformOutboundListener();

  /* Schedules a task to inform the network listener immediately that the Ticl
   * has outbound data waiting to be sent.
   */
  void DoInformOutboundListener();

  /* The network endpoint through which the application and Ticl communicate.
   */
  NetworkEndpoint* endpoint_;

  /* System resources (for scheduling and logging). */
  SystemResources* resources_;

  /* A rate-limiter for calls to inform the network listenr that we have data to
   * send.
   */
  Throttle throttle_;

  /* Whether or not we have useful data for the server. */
  bool has_outbound_data_;

  /* A callback to call when an outbound message is ready, or null. */
  NetworkCallback* outbound_listener_;

  /* The last time we polled for invalidations, as reported by the system
   * resources.
   */
  Time last_poll_;

  /* The last time we sent any message to the server, as reported by the system
   * resources.
   */
  Time last_send_;

  /* How long we should wait between polling for invalidations. */
  TimeDelta poll_delay_;

  /* How long we should wait before sending a message (assuming no additional
   * message content to send).
   */
  TimeDelta heartbeat_delay_;

  /* The maximum delay for the timer that checks whether to send a heartbeat.
   */
  static const int MAX_TIMER_DELAY_MS;

  friend class InvalidationClientImpl;
};

}  // namespace invalidation

#endif  // GOOGLE_CACHEINVALIDATION_NETWORK_MANAGER_H_
