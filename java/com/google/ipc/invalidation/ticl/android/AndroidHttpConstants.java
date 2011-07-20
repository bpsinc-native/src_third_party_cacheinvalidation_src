/*
 * Copyright 2011 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.google.ipc.invalidation.ticl.android;

/**
 * Defines shared constants values used in the Android Invalidation HTTP interface.
 *
 */
public class AndroidHttpConstants {

  /** The name of the service that should be used for auth token acquisition/validation */
  // TODO: Change to real value once allocated by Gaia team
  public static final String SERVICE = "chromiumsync";

  /** The MIME content type to use for requests that contain binary protobuf */
  public static final String PROTO_CONTENT_TYPE = "application/x-protobuffer";

  /** The relative URL to use to send inbound client requests to the Android frontend */
  public static final String REQUEST_URL = "/invalidation/android/request/";

  /** The relative URL to use to send mailbox retrieval requests to the Android frontend */
  public static final String MAILBOX_URL = "/invalidation/android/mailbox/";
}
