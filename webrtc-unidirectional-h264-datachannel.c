#include <locale.h>
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define RTP_PAYLOAD_TYPE "96"
#define RTP_AUDIO_PAYLOAD_TYPE "97"
#define SOUP_HTTP_PORT 57778
#define STUN_SERVER "stun.l.google.com:19302"

#ifdef G_OS_WIN32
#define VIDEO_SRC "mfvideosrc"
#else
#define VIDEO_SRC "videotestsrc"
#endif

gchar *video_priority = NULL;
gchar *audio_priority = NULL;


typedef struct _ReceiverEntry ReceiverEntry;

ReceiverEntry *create_receiver_entry (SoupWebsocketConnection * connection);
void destroy_receiver_entry (gpointer receiver_entry_ptr);

void on_offer_created_cb (GstPromise * promise, gpointer user_data);
void on_negotiation_needed_cb (GstElement * webrtcbin, gpointer user_data);
void on_ice_candidate_cb (GstElement * webrtcbin, guint mline_index,
    gchar * candidate, gpointer user_data);

void on_notify_connection_state_cb (GstElement * webrtcbin, GParamSpec *arg1, gpointer user_data);
void on_notify_signaling_state_cb (GstElement * webrtcbin,  GParamSpec *arg1, gpointer user_data);


void soup_websocket_message_cb (SoupWebsocketConnection * connection,
    SoupWebsocketDataType data_type, GBytes * message, gpointer user_data);
void soup_websocket_closed_cb (SoupWebsocketConnection * connection,
    gpointer user_data);

void soup_http_handler (SoupServer * soup_server, SoupMessage * message,
    const char *path, GHashTable * query, SoupClientContext * client_context,
    gpointer user_data);
void soup_websocket_handler (G_GNUC_UNUSED SoupServer * server,
    SoupWebsocketConnection * connection, const char *path,
    SoupClientContext * client_context, gpointer user_data);

static gchar *get_string_from_json_object (JsonObject * object);

struct _ReceiverEntry
{
  SoupWebsocketConnection *connection;

  GstElement *pipeline;
  GstElement *webrtcbin;
  int making_offer;
  int polite;
};


static gboolean
bus_watch_cb (GstBus * bus, GstMessage * message, gpointer user_data)
{
  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:
    {
      GError *error = NULL;
      gchar *debug = NULL;

      gst_message_parse_error (message, &error, &debug);
      g_error ("Error on bus: %s (debug: %s)", error->message, debug);
      g_error_free (error);
      g_free (debug);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      GError *error = NULL;
      gchar *debug = NULL;

      gst_message_parse_warning (message, &error, &debug);
      g_warning ("Warning on bus: %s (debug: %s)", error->message, debug);
      g_error_free (error);
      g_free (debug);
      break;
    }
    default:
      break;
  }

  return G_SOURCE_CONTINUE;
}

static GstWebRTCPriorityType
_priority_from_string (const gchar * s)
{
  GEnumClass *klass =
      (GEnumClass *) g_type_class_ref (GST_TYPE_WEBRTC_PRIORITY_TYPE);
  GEnumValue *en;

  g_return_val_if_fail (klass, 0);
  if (!(en = g_enum_get_value_by_name (klass, s)))
    en = g_enum_get_value_by_nick (klass, s);
  g_type_class_unref (klass);

  if (en)
    return en->value;

  return 0;
}

void data_channel_on_error_cb(GstWebRTCDataChannel * self,
    GError * error,
    gpointer user_data) {

  gst_print("data_channel_on_error_cb()\n");

}

void data_channel_on_open_cb(GstWebRTCDataChannel * self,
    gpointer user_data) {
    gst_print("data_channel_on_open_cb()\n");

}

void data_channel_on_close_cb(GstWebRTCDataChannel * self,
    gpointer user_data) {
  gst_print("data_channel_on_close_cb()\n");

}

void data_channel_on_message_string_cb (GstWebRTCDataChannel * self,
                            gchar * data,
                            gpointer user_data) {
  gst_print("data_channel_on_message_string_cb() : %s\n", data);
  gst_webrtc_data_channel_send_string(self, "YadaYada!");
}



void data_channel_on_message_data_cb (GstWebRTCDataChannel * self,
                          GBytes * data,
                          gpointer user_data) {
  gst_print("data_channel_on_message_data_cb()\n");

}

void incomingDataChannelAdded(GstElement * object,
    GstWebRTCDataChannel * candidate, ReceiverEntry* receiver_entry) {

  gst_print("Data channel added.\n");

  g_signal_connect(candidate, "on-error",
      G_CALLBACK (data_channel_on_error_cb), receiver_entry);

  g_signal_connect(candidate, "on-open",
      G_CALLBACK (data_channel_on_open_cb), receiver_entry);

  g_signal_connect(candidate, "on-close",
      G_CALLBACK (data_channel_on_close_cb), receiver_entry);

  g_signal_connect(candidate, "on-message-string",
      G_CALLBACK (data_channel_on_message_string_cb), receiver_entry);

  g_signal_connect(candidate, "on-message-data",
      G_CALLBACK (data_channel_on_message_data_cb), receiver_entry);
}

void on_new_transceiver_callback (GstElement * object,
                             GstWebRTCRTPTransceiver * candidate,
                             gpointer udata) {
  GST_WARNING("on_new_transceiver_callback.");
}

ReceiverEntry *
create_receiver_entry (SoupWebsocketConnection * connection)
{
  GError *error;
  ReceiverEntry *receiver_entry;
  GstWebRTCRTPTransceiver *trans;
  GArray *transceivers;
  GstBus *bus;

  gst_print ("create_receiver_entry()\n");


  receiver_entry = g_slice_alloc0 (sizeof (ReceiverEntry));
  receiver_entry->connection = connection;

  g_object_ref (G_OBJECT (connection));

  g_signal_connect (G_OBJECT (connection), "message",
      G_CALLBACK (soup_websocket_message_cb), (gpointer) receiver_entry);

  error = NULL;
  receiver_entry->pipeline =
      gst_parse_launch ("webrtcbin name=webrtcbin stun-server=stun://"
      STUN_SERVER " "
      VIDEO_SRC
      " ! videorate ! videoscale ! video/x-raw,width=640,height=360,framerate=15/1 ! videoconvert ! queue max-size-buffers=1 ! x264enc bitrate=600 speed-preset=ultrafast tune=zerolatency key-int-max=15 ! video/x-h264,profile=constrained-baseline ! queue max-size-time=100000000 ! h264parse ! "
      "rtph264pay config-interval=-1 name=payloader aggregate-mode=zero-latency ! "
      "application/x-rtp,media=video,encoding-name=H264,payload="
      RTP_PAYLOAD_TYPE " ! webrtcbin. "
      "audiotestsrc ! queue max-size-buffers=1 leaky=downstream ! audioconvert ! audioresample ! opusenc ! rtpopuspay pt="
      RTP_AUDIO_PAYLOAD_TYPE " ! webrtcbin. ", &error);
  if (error != NULL) {
    g_error ("Could not create WebRTC pipeline: %s\n", error->message);
    g_error_free (error);
    goto cleanup;
  }

  receiver_entry->webrtcbin =
      gst_bin_get_by_name (GST_BIN (receiver_entry->pipeline), "webrtcbin");
  g_assert (receiver_entry->webrtcbin != NULL);

  receiver_entry->making_offer = 0;
  receiver_entry->polite = 1; // The javascript implementation is the im-polite side.

  g_signal_emit_by_name (receiver_entry->webrtcbin, "get-transceivers",
      &transceivers);
  g_assert (transceivers != NULL && transceivers->len > 1);
  trans = g_array_index (transceivers, GstWebRTCRTPTransceiver *, 0);
  g_object_set (trans, "direction",
      GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
  if (video_priority) {
    GstWebRTCPriorityType priority;

    priority = _priority_from_string (video_priority);
    if (priority) {
      GstWebRTCRTPSender *sender;

      g_object_get (trans, "sender", &sender, NULL);
      gst_webrtc_rtp_sender_set_priority (sender, priority);
      g_object_unref (sender);
    }
  }
  trans = g_array_index (transceivers, GstWebRTCRTPTransceiver *, 1);
  g_object_set (trans, "direction",
      GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, NULL);
  if (audio_priority) {
    GstWebRTCPriorityType priority;

    priority = _priority_from_string (audio_priority);
    if (priority) {
      GstWebRTCRTPSender *sender;

      g_object_get (trans, "sender", &sender, NULL);
      gst_webrtc_rtp_sender_set_priority (sender, priority);
      g_object_unref (sender);
    }
  }
  g_array_unref (transceivers);

  g_signal_connect (receiver_entry->webrtcbin, "on-negotiation-needed",
      G_CALLBACK (on_negotiation_needed_cb), (gpointer) receiver_entry);

  g_signal_connect (receiver_entry->webrtcbin, "on-ice-candidate",
      G_CALLBACK (on_ice_candidate_cb), (gpointer) receiver_entry);

  g_signal_connect(receiver_entry->webrtcbin, "on-data-channel",
      G_CALLBACK (incomingDataChannelAdded), receiver_entry);

  g_signal_connect(receiver_entry->webrtcbin, "on-new-transceiver",
      G_CALLBACK (on_new_transceiver_callback), receiver_entry);

  g_signal_connect(receiver_entry->webrtcbin, "notify::connection-state",
      G_CALLBACK (on_notify_connection_state_cb), (gpointer) receiver_entry);

  g_signal_connect(receiver_entry->webrtcbin, "notify::signaling-state",
      G_CALLBACK (on_notify_signaling_state_cb), (gpointer) receiver_entry);

  gst_print ("receiver_entry @ 0x%08X\n", receiver_entry);


  bus = gst_pipeline_get_bus (GST_PIPELINE (receiver_entry->pipeline));
  gst_bus_add_watch (bus, bus_watch_cb, NULL);
  gst_object_unref (bus);

  if (gst_element_set_state (receiver_entry->pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    g_error ("Could not start pipeline");

  return receiver_entry;

cleanup:
  destroy_receiver_entry ((gpointer) receiver_entry);
  return NULL;
}

void
destroy_receiver_entry (gpointer receiver_entry_ptr)
{
  ReceiverEntry *receiver_entry = (ReceiverEntry *) receiver_entry_ptr;

  g_assert (receiver_entry != NULL);

  if (receiver_entry->pipeline != NULL) {
    gst_element_set_state (GST_ELEMENT (receiver_entry->pipeline),
        GST_STATE_NULL);

    gst_object_unref (GST_OBJECT (receiver_entry->webrtcbin));
    gst_object_unref (GST_OBJECT (receiver_entry->pipeline));
  }

  if (receiver_entry->connection != NULL)
    g_object_unref (G_OBJECT (receiver_entry->connection));

  g_slice_free1 (sizeof (ReceiverEntry), receiver_entry);
}


void
on_offer_created_cb (GstPromise * promise, gpointer user_data)
{
  gchar *sdp_string;
  gchar *json_string;
  JsonObject *sdp_json;
  JsonObject *sdp_data_json;
  GstStructure const *reply;
  GstPromise *local_desc_promise;
  GstWebRTCSessionDescription *offer = NULL;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;
  gst_print ("on_offer_created_cb()\n");
  gst_print ("receiver_entry @ 0x%08X\n", receiver_entry);

  reply = gst_promise_get_reply (promise);
  gst_structure_get (reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
      &offer, NULL);
  gst_promise_unref (promise);

  local_desc_promise = gst_promise_new ();
  g_signal_emit_by_name (receiver_entry->webrtcbin, "set-local-description",
      offer, local_desc_promise);
  gst_promise_interrupt (local_desc_promise);
  gst_promise_unref (local_desc_promise);

  sdp_string = gst_sdp_message_as_text (offer->sdp);
  gst_print ("Negotiation offer created:\n%s\n", sdp_string);

  sdp_json = json_object_new ();
  json_object_set_string_member (sdp_json, "type", "sdp");

  sdp_data_json = json_object_new ();
  json_object_set_string_member (sdp_data_json, "type", "offer");
  json_object_set_string_member (sdp_data_json, "sdp", sdp_string);
  json_object_set_object_member (sdp_json, "data", sdp_data_json);

  json_string = get_string_from_json_object (sdp_json);
  json_object_unref (sdp_json);

  soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  g_free (json_string);
  g_free (sdp_string);

  gst_webrtc_session_description_free (offer);
  receiver_entry->making_offer = 0;

}


void
on_negotiation_needed_cb (GstElement * webrtcbin, gpointer user_data)
{
  GstPromise *promise;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;
  gst_print ("on_negotiation_needed_cb\n");
  gst_print ("receiver_entry @ 0x%08X\n", receiver_entry);

  receiver_entry->making_offer = 1;
  promise = gst_promise_new_with_change_func (on_offer_created_cb,
      (gpointer) receiver_entry, NULL);
  g_signal_emit_by_name (G_OBJECT (webrtcbin), "create-offer", NULL, promise);

}


void
on_ice_candidate_cb (G_GNUC_UNUSED GstElement * webrtcbin, guint mline_index,
    gchar * candidate, gpointer user_data)
{
  JsonObject *ice_json;
  JsonObject *ice_data_json;
  gchar *json_string;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;
  gst_print ("on_ice_candidate_cb\n");
  gst_print ("receiver_entry @ 0x%08X\n", receiver_entry);
  ice_json = json_object_new ();
  json_object_set_string_member (ice_json, "type", "ice");

  ice_data_json = json_object_new ();
  json_object_set_int_member (ice_data_json, "sdpMLineIndex", mline_index);
  json_object_set_string_member (ice_data_json, "candidate", candidate);
  json_object_set_object_member (ice_json, "data", ice_data_json);

  json_string = get_string_from_json_object (ice_json);
  json_object_unref (ice_json);

  soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  g_free (json_string);
}

void on_notify_connection_state_cb (GstElement * webrtcbin, GParamSpec *arg1, gpointer user_data) {

  gst_print ("on_notify_connection_state_cb()\n");
  gst_print ("user_data @ 0x%08X\n", user_data);

}
void on_local_description_set_cb (GstPromise * promise, gpointer user_data) {
  gst_print ("on_local_description_set_cb()\n");

}
void set_local_description(GstWebRTCSessionDescription *description, GstElement * webrtcbin, gpointer user_data) {
  gst_print ("set_local_description()\n");

  GstPromise *promise = gst_promise_new_with_change_func(
      on_local_description_set_cb, user_data, NULL);
  g_signal_emit_by_name(webrtcbin, "set-local-description",
                        description, promise);
}

void on_answer_created_cb (GstPromise * promise, gpointer user_data) {
  gst_print ("on_answer_created_cb()\n");
  gst_print ("user_data @ 0x%08X\n", user_data);

  gchar *json_string;
  JsonObject *sdp_json;
  JsonObject *sdp_data_json;

  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;
  gst_print ("receiver_entry @ 0x%08X\n", receiver_entry);

  GstStructure *reply = gst_promise_get_reply(promise);
  GstWebRTCSessionDescription* answer = NULL;
  gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &answer, NULL);
  gst_promise_unref(promise);

  set_local_description(answer, receiver_entry->webrtcbin, user_data);

  gchar* sdp_string = gst_sdp_message_as_text(answer->sdp);
  gst_print ("sending answer : \n%s\n", sdp_string);

  sdp_json = json_object_new ();
  json_object_set_string_member (sdp_json, "type", "sdp");

  sdp_data_json = json_object_new ();
  json_object_set_string_member (sdp_data_json, "type", "answer");
  json_object_set_string_member (sdp_data_json, "sdp", sdp_string);
  json_object_set_object_member (sdp_json, "data", sdp_data_json);

  json_string = get_string_from_json_object (sdp_json);
  json_object_unref (sdp_json);

  soup_websocket_connection_send_text (receiver_entry->connection, json_string);
  g_free (json_string);
  g_free (sdp_string);


  gst_webrtc_session_description_free(answer);
}

void create_answer(GstElement * webrtcbin, gpointer user_data) {
  gst_print ("create_answer()\n");
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  GstPromise *promise = gst_promise_new_with_change_func(
      on_answer_created_cb, user_data, NULL);
  g_signal_emit_by_name(webrtcbin, "create-answer", NULL, promise);
}

void on_local_negotiation_requested() {
  gst_print ("on_local_negotiation_requested() ToDO\n");
}

void mark_connected() {
  gst_print ("mark_connected()\n");
}

void signalling_state_stable(gpointer user_data) {
  gst_print ("signalling_state_stable() ToDO\n");
}

void on_notify_signaling_state_cb (GstElement * webrtcbin,  GParamSpec *arg1, gpointer user_data) {
  gst_print ("on_notify_signaling_state_cb()\n");
  gst_print ("user_data @ 0x%08X\n", user_data);

  GstWebRTCSignalingState state;
  g_object_get (webrtcbin, "signaling-state", &state, NULL);
  gst_print ("State : %u\n", state);


  switch (state) {
    case GST_WEBRTC_SIGNALING_STATE_STABLE:
      signalling_state_stable(user_data);
      break;
    case GST_WEBRTC_SIGNALING_STATE_CLOSED:
      break;
    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_OFFER:
      create_answer(webrtcbin, user_data);
      break;
    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_OFFER:
    case GST_WEBRTC_SIGNALING_STATE_HAVE_LOCAL_PRANSWER:
    case GST_WEBRTC_SIGNALING_STATE_HAVE_REMOTE_PRANSWER:
      break;
  }
}

void
soup_websocket_message_cb (G_GNUC_UNUSED SoupWebsocketConnection * connection,
    SoupWebsocketDataType data_type, GBytes * message, gpointer user_data)
{
  gsize size;
  gchar *data;
  gchar *data_string;
  const gchar *type_string;
  JsonNode *root_json;
  JsonObject *root_json_object;
  JsonObject *data_json_object;
  JsonParser *json_parser = NULL;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) user_data;

  switch (data_type) {
    case SOUP_WEBSOCKET_DATA_BINARY:
      g_error ("Received unknown binary message, ignoring\n");
      g_bytes_unref (message);
      return;

    case SOUP_WEBSOCKET_DATA_TEXT:
      data = g_bytes_unref_to_data (message, &size);
      /* Convert to NULL-terminated string */
      data_string = g_strndup (data, size);
      g_free (data);
      break;

    default:
      g_assert_not_reached ();
  }

  json_parser = json_parser_new ();
  if (!json_parser_load_from_data (json_parser, data_string, -1, NULL))
    goto unknown_message;

  root_json = json_parser_get_root (json_parser);
  if (!JSON_NODE_HOLDS_OBJECT (root_json))
    goto unknown_message;

  root_json_object = json_node_get_object (root_json);

  if (!json_object_has_member (root_json_object, "type")) {
    g_error ("Received message without type field\n");
    goto cleanup;
  }
  type_string = json_object_get_string_member (root_json_object, "type");

  if (!json_object_has_member (root_json_object, "data")) {
    g_error ("Received message without data field\n");
    goto cleanup;
  }
  data_json_object = json_object_get_object_member (root_json_object, "data");

  if (g_strcmp0 (type_string, "sdp") == 0) {
    const gchar *sdp_type_string;
    const gchar *sdp_string;
    GstPromise *promise;
    GstSDPMessage *sdp;
    GstWebRTCSessionDescription *answer_or_offer;
    int ret;

    GstWebRTCSignalingState signaling_state;
    g_object_get (receiver_entry->webrtcbin, "signaling-state", &signaling_state, NULL);
    gst_print ("signaling-state : %u\n", signaling_state);


    if (!json_object_has_member (data_json_object, "type")) {
      g_error ("Received SDP message without type field\n");
      goto cleanup;
    }
    sdp_type_string = json_object_get_string_member (data_json_object, "type");

    if (!json_object_has_member (data_json_object, "sdp")) {
      g_error ("Received SDP message without SDP string\n");
      goto cleanup;
    }
    sdp_string = json_object_get_string_member (data_json_object, "sdp");

    gst_print ("Received SDP %s:\n%s\n", sdp_type_string, sdp_string);

    ret = gst_sdp_message_new (&sdp);
    g_assert_cmphex (ret, ==, GST_SDP_OK);

    ret =
        gst_sdp_message_parse_buffer ((guint8 *) sdp_string,
        strlen (sdp_string), sdp);
    if (ret != GST_SDP_OK) {
      g_error ("Could not parse SDP string\n");
      goto cleanup;
    }

    if (g_strcmp0 (sdp_type_string, "answer") == 0)
      answer_or_offer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_ANSWER,
          sdp);
    else if(g_strcmp0 (sdp_type_string, "offer") == 0) {
      int offer_collision = (signaling_state != GST_WEBRTC_SIGNALING_STATE_STABLE) || receiver_entry->making_offer;
      int ignore_offer =  (!receiver_entry->polite) && offer_collision;
      if(ignore_offer) {
        gst_print ("Ignoring Offer\n");
        goto cleanup;
      }
      answer_or_offer = gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER,
          sdp);
    }
    else {
      g_error ("Expected SDP message type \"answer\" or \"offer\", got \"%s\"\n",
          sdp_type_string);
      goto cleanup;
    }


    g_assert_nonnull (answer_or_offer);

    promise = gst_promise_new ();
    g_signal_emit_by_name (receiver_entry->webrtcbin, "set-remote-description",
        answer_or_offer, promise);
    gst_promise_interrupt (promise);
    gst_promise_unref (promise);
    gst_webrtc_session_description_free (answer_or_offer);
  } else if (g_strcmp0 (type_string, "ice") == 0) {
    guint mline_index;
    const gchar *candidate_string;

    if (!json_object_has_member (data_json_object, "sdpMLineIndex")) {
      g_error ("Received ICE message without mline index\n");
      goto cleanup;
    }
    mline_index =
        json_object_get_int_member (data_json_object, "sdpMLineIndex");

    if (!json_object_has_member (data_json_object, "candidate")) {
      g_error ("Received ICE message without ICE candidate string\n");
      goto cleanup;
    }
    candidate_string = json_object_get_string_member (data_json_object,
        "candidate");

    gst_print ("Received ICE candidate with mline index %u; candidate: %s\n",
        mline_index, candidate_string);

    g_signal_emit_by_name (receiver_entry->webrtcbin, "add-ice-candidate",
        mline_index, candidate_string);
  } else
    goto unknown_message;

cleanup:
  if (json_parser != NULL)
    g_object_unref (G_OBJECT (json_parser));
  g_free (data_string);
  return;

unknown_message:
  g_error ("Unknown message \"%s\", ignoring", data_string);
  goto cleanup;
}


void
soup_websocket_closed_cb (SoupWebsocketConnection * connection,
    gpointer user_data)
{
  GHashTable *receiver_entry_table = (GHashTable *) user_data;
  ReceiverEntry *receiver_entry = (ReceiverEntry *) g_hash_table_lookup(receiver_entry_table, connection);
  g_hash_table_remove (receiver_entry_table, connection);
  gst_print ("Closed websocket connection %p\n", (gpointer) connection);


  gst_element_set_state(receiver_entry->webrtcbin, GST_STATE_NULL);
}


void
soup_http_handler (G_GNUC_UNUSED SoupServer * soup_server,
    SoupMessage * message, const char *path, G_GNUC_UNUSED GHashTable * query,
    G_GNUC_UNUSED SoupClientContext * client_context,
    G_GNUC_UNUSED gpointer user_data)
{
  SoupBuffer *soup_buffer;

  if ((g_strcmp0 (path, "/") != 0) && (g_strcmp0 (path, "/index.html") != 0)) {
    soup_message_set_status (message, SOUP_STATUS_NOT_FOUND);
    return;
  }

//  soup_buffer =
//      soup_buffer_new (SOUP_MEMORY_STATIC, html_source, strlen (html_source));
//
//  soup_message_headers_set_content_type (message->response_headers, "text/html",
//      NULL);
//  soup_message_body_append_buffer (message->response_body, soup_buffer);
//  soup_buffer_free (soup_buffer);


  GMappedFile *mapping;
  SoupBuffer *buffer;

  mapping = g_mapped_file_new ("./webrtc-unidirectional-h264-datachannel.html", FALSE, NULL);
  if (!mapping) {
    soup_message_set_status (message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
    return;
  }

  buffer = soup_buffer_new_with_owner (g_mapped_file_get_contents (mapping),
               g_mapped_file_get_length (mapping),
               mapping, (GDestroyNotify)g_mapped_file_unref);
  soup_message_body_append_buffer (message->response_body, buffer);
  soup_buffer_free (buffer);

  soup_message_set_status (message, SOUP_STATUS_OK);
}


void
soup_websocket_handler (G_GNUC_UNUSED SoupServer * server,
    SoupWebsocketConnection * connection, G_GNUC_UNUSED const char *path,
    G_GNUC_UNUSED SoupClientContext * client_context, gpointer user_data)
{
  ReceiverEntry *receiver_entry;
  GHashTable *receiver_entry_table = (GHashTable *) user_data;

  gst_print ("Processing new websocket connection %p\n", (gpointer) connection);

  g_signal_connect (G_OBJECT (connection), "closed",
      G_CALLBACK (soup_websocket_closed_cb), (gpointer) receiver_entry_table);

  receiver_entry = create_receiver_entry (connection);
  g_hash_table_replace (receiver_entry_table, connection, receiver_entry);
}


static gchar *
get_string_from_json_object (JsonObject * object)
{
  JsonNode *root;
  JsonGenerator *generator;
  gchar *text;

  /* Make it the root node */
  root = json_node_init_object (json_node_alloc (), object);
  generator = json_generator_new ();
  json_generator_set_root (generator, root);
  text = json_generator_to_data (generator, NULL);

  /* Release everything */
  g_object_unref (generator);
  json_node_free (root);
  return text;
}

#ifdef G_OS_UNIX
gboolean
exit_sighandler (gpointer user_data)
{
  gst_print ("Caught signal, stopping mainloop\n");
  GMainLoop *mainloop = (GMainLoop *) user_data;
  g_main_loop_quit (mainloop);
  return TRUE;
}
#endif


static GOptionEntry entries[] = {
  {"video-priority", 0, 0, G_OPTION_ARG_STRING, &video_priority,
        "Priority of the video stream (very-low, low, medium or high)",
      "PRIORITY"},
  {"audio-priority", 0, 0, G_OPTION_ARG_STRING, &audio_priority,
        "Priority of the audio stream (very-low, low, medium or high)",
      "PRIORITY"},
  {NULL},
};

int
main (int argc, char *argv[])
{
  GMainLoop *mainloop;
  SoupServer *soup_server;
  GHashTable *receiver_entry_table;
  GOptionContext *context;
  GError *error = NULL;

  setlocale (LC_ALL, "");

  context = g_option_context_new ("- gstreamer webrtc sendonly demo");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group ());
  if (!g_option_context_parse (context, &argc, &argv, &error)) {
    g_printerr ("Error initializing: %s\n", error->message);
    return -1;
  }

  receiver_entry_table =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      destroy_receiver_entry);

  mainloop = g_main_loop_new (NULL, FALSE);
  g_assert (mainloop != NULL);

#ifdef G_OS_UNIX
  g_unix_signal_add (SIGINT, exit_sighandler, mainloop);
  g_unix_signal_add (SIGTERM, exit_sighandler, mainloop);
#endif

  soup_server =
      soup_server_new (SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server", NULL);
  soup_server_add_handler (soup_server, "/", soup_http_handler, NULL, NULL);
  soup_server_add_websocket_handler (soup_server, "/ws", NULL, NULL,
      soup_websocket_handler, (gpointer) receiver_entry_table, NULL);
  soup_server_listen_all (soup_server, SOUP_HTTP_PORT,
      (SoupServerListenOptions) 0, NULL);

  gst_print ("WebRTC page link: http://127.0.0.1:%d/\n", (gint) SOUP_HTTP_PORT);

  g_main_loop_run (mainloop);

  g_object_unref (G_OBJECT (soup_server));
  g_hash_table_destroy (receiver_entry_table);
  g_main_loop_unref (mainloop);

  gst_deinit ();

  return 0;
}
