#include <cstdlib>

#include <switch.h>
#include <switch_json.h>
#include <grpc++/grpc++.h>

#include "google/cloud/speech/v1/cloud_speech.grpc.pb.h"

#include "mod_google_transcribe.h"

#define BUFFER_SECS (3)

using google::cloud::speech::v1::RecognitionConfig;
using google::cloud::speech::v1::Speech;
using google::cloud::speech::v1::StreamingRecognizeRequest;
using google::cloud::speech::v1::StreamingRecognizeResponse;

class GStreamer;

class GStreamer {
public:
	GStreamer(switch_core_session_t *session, u_int16_t channels, char* lang, int interim) : m_session(session) {
		m_creds = grpc::GoogleDefaultCredentials();
  	m_channel = grpc::CreateChannel("speech.googleapis.com", m_creds);
  	m_stub = Speech::NewStub(m_channel);
  		
		auto* streaming_config = m_request.mutable_streaming_config();
		RecognitionConfig* config = streaming_config->mutable_config();
		config->set_language_code(lang);
  	config->set_sample_rate_hertz(16000);
		config->set_encoding(RecognitionConfig::LINEAR16);

  	// Begin a stream.
  	m_streamer = m_stub->StreamingRecognize(&m_context);

  	// Write the first request, containing the config only.
  	streaming_config->set_interim_results(interim);
  	m_streamer->Write(m_request);
	}

	~GStreamer() {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(m_session), SWITCH_LOG_DEBUG, "GStreamer::~GStreamer - deleting channel and stub\n");
	}

	bool write(void* data, uint32_t datalen) {
    m_request.set_audio_content(data, datalen);
    bool ok = m_streamer->Write(m_request);
    return ok;
  }

	uint32_t nextMessageSize(void) {
		uint32_t size = 0;
		m_streamer->NextMessageSize(&size);
		return size;
	}

	bool read(StreamingRecognizeResponse* response) {
		return m_streamer->Read(response);
	}

	grpc::Status finish() {
		return m_streamer->Finish();
	}

	void writesDone() {
		m_streamer->WritesDone();
	}

protected:

private:
	switch_core_session_t* m_session;
  grpc::ClientContext m_context;
	std::shared_ptr<grpc::ChannelCredentials> m_creds;
	std::shared_ptr<grpc::Channel> m_channel;
	std::unique_ptr<Speech::Stub> 	m_stub;
	std::unique_ptr< grpc::ClientReaderWriterInterface<StreamingRecognizeRequest, StreamingRecognizeResponse> > m_streamer;
	StreamingRecognizeRequest m_request;
};

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
	struct cap_cb *cb = (struct cap_cb *) obj;
	GStreamer* streamer = (GStreamer *) cb->streamer;

  // Read responses.
  StreamingRecognizeResponse response;
  while (streamer->read(&response)) {  // Returns false when no more to read.
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: got %d responses\n", response.results_size());

    for (int r = 0; r < response.results_size(); ++r) {
      auto result = response.results(r);
      cJSON * jResult = cJSON_CreateObject();
      cJSON * jAlternatives = cJSON_CreateArray();
      cJSON * jStability = cJSON_CreateNumber(result.stability());
      cJSON * jIsFinal = cJSON_CreateBool(result.is_final());

      cJSON_AddItemToObject(jResult, "stability", jStability);
      cJSON_AddItemToObject(jResult, "is_final", jIsFinal);
      cJSON_AddItemToObject(jResult, "alternatives", jAlternatives);

    for (int a = 0; a < result.alternatives_size(); ++a) {
      auto alternative = result.alternatives(a);
        cJSON* jAlt = cJSON_CreateObject();
        cJSON* jConfidence = cJSON_CreateNumber(alternative.confidence());
        cJSON* jTranscript = cJSON_CreateString(alternative.transcript().c_str());
        cJSON_AddItemToObject(jAlt, "confidence", jConfidence);
        cJSON_AddItemToObject(jAlt, "transcript", jTranscript);
        cJSON_AddItemToArray(jAlternatives, jAlt);
      }

      char* json = cJSON_PrintUnformatted(jResult);
      cb->responseHandler(cb->session, json);
      free(json);

      cJSON_Delete(jResult);
    }
  }
  grpc::Status status = streamer->finish();
}

extern "C" {
    switch_status_t google_speech_init() {
      const char* gcsServiceKeyFile = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
      if (NULL == gcsServiceKeyFile) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, 
          "Error: \"GOOGLE_APPLICATION_CREDENTIALS\" environment variable must be set to path of the file containing service account json key\n");
        return SWITCH_STATUS_FALSE;     
      }
      try {
        auto creds = grpc::GoogleDefaultCredentials();
      } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, 
          "Error initializing google api with provided credentials in %s: %s\n", gcsServiceKeyFile, e.what());
        return SWITCH_STATUS_FALSE;
      }
      return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t google_speech_cleanup() {
      return SWITCH_STATUS_SUCCESS;
    }
    switch_status_t google_speech_session_init(switch_core_session_t *session, responseHandler_t responseHandler, 
		  uint32_t samples_per_second, uint32_t channels, char* lang, int interim, void **ppUserData) {
    	
		  switch_channel_t *channel = switch_core_session_get_channel(session);
    	struct cap_cb *cb;
      int err;

    	cb =(struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
    	cb->base = switch_core_session_strdup(session, "mod_google_transcribe");
      cb->session = session;

	    switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

      cb->resampler = speex_resampler_init(channels, 8000, 16000, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n", 
          switch_channel_get_name(channel), speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }

      GStreamer *streamer = NULL;
      try {
        streamer = new GStreamer(session, channels, lang, interim);
        cb->streamer = streamer;
      } catch (std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing gstreamer: %s.\n", 
          switch_channel_get_name(channel), e.what());
        return SWITCH_STATUS_FALSE;
      }

      cb->responseHandler = responseHandler;

      // create the read thread
      switch_threadattr_t *thd_attr = NULL;
      switch_memory_pool_t *pool = switch_core_session_get_pool(session);

      switch_threadattr_create(&thd_attr, pool);
      switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
      switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

      *ppUserData = cb;
      return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t google_speech_session_cleanup(switch_core_session_t *session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);

      if (bug) {
        struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
        switch_mutex_lock(cb->mutex);

        // close connection and get final responses
        GStreamer* streamer = (GStreamer *) cb->streamer;
        streamer->writesDone();

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_speech_session_cleanup: waiting for read thread to complete\n");
        switch_status_t st;
        switch_thread_join(&st, cb->thread);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_speech_session_cleanup: read thread completed\n");

        delete streamer;
        cb->streamer = NULL;
        speex_resampler_destroy(cb->resampler);
        switch_channel_set_private(channel, MY_BUG_NAME, NULL);
			  switch_mutex_unlock(cb->mutex);

			  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_speech_session_cleanup: Closed streamile\n");

			  return SWITCH_STATUS_SUCCESS;
      }

      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
      return SWITCH_STATUS_FALSE;
    }

    switch_bool_t google_speech_frame(switch_media_bug_t *bug, void* user_data) {
    	switch_core_session_t *session = switch_core_media_bug_get_session(bug);
    	struct cap_cb *cb = (struct cap_cb *) user_data;
		  if (cb->streamer) {
        GStreamer* streamer = (GStreamer *) cb->streamer;
        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
          while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
            if (frame.datalen) {
              spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
              spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
              spx_uint32_t in_len = frame.samples;
              size_t written;
              
              speex_resampler_process_interleaved_int(cb->resampler, 
                (const spx_int16_t *) frame.data, 
                (spx_uint32_t *) &in_len, 
                &out[0], 
                &out_len);
                          
              streamer->write( &out[0], sizeof(spx_int16_t) * out_len);
            }
          }
          switch_mutex_unlock(cb->mutex);
        }
      }
      return SWITCH_TRUE;
    }
}