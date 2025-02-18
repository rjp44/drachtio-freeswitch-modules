#include <cstdlib>

#include <switch.h>
#include <switch_json.h>
#include <grpc++/grpc++.h>
#include <string.h>
#include <mutex>
#include <condition_variable>

#include <fstream>
#include <string>

#include "google/cloud/dialogflow/v2beta1/session.grpc.pb.h"

#include "mod_dialogflow.h"
#include "parser.h"

using google::cloud::dialogflow::v2beta1::Sessions;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentRequest;
using google::cloud::dialogflow::v2beta1::StreamingDetectIntentResponse;
using google::cloud::dialogflow::v2beta1::AudioEncoding;
using google::cloud::dialogflow::v2beta1::InputAudioConfig;
using google::cloud::dialogflow::v2beta1::OutputAudioConfig;
using google::cloud::dialogflow::v2beta1::SynthesizeSpeechConfig;
using google::cloud::dialogflow::v2beta1::QueryInput;
using google::cloud::dialogflow::v2beta1::QueryResult;
using google::cloud::dialogflow::v2beta1::StreamingRecognitionResult;
using google::cloud::dialogflow::v2beta1::EventInput;
using google::rpc::Status;
using google::protobuf::Struct;

static uint64_t playCount = 0;

class GStreamer {
public:
	GStreamer(switch_core_session_t *session, const char* lang, char* projectId, char* event) : 
	m_lang(lang), m_projectId(projectId), m_sessionId(switch_core_session_get_uuid(session)), m_finished(false), m_packets(0),
	m_creds(grpc::GoogleDefaultCredentials()), m_channel(grpc::CreateChannel("dialogflow.googleapis.com", m_creds))
 {
		startStream(session, event);
	}

	~GStreamer() {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::~GStreamer wrote %ld packets %p\n", m_packets, this);		
	}

	void startStream(switch_core_session_t *session, const char* event) {
		char szSession[256];
		m_request = std::make_shared<StreamingDetectIntentRequest>();
		m_context= std::make_shared<grpc::ClientContext>();
		m_stub = Sessions::NewStub(m_channel);
		snprintf(szSession, 256, "projects/%s/agent/sessions/%s", m_projectId.c_str(), m_sessionId.c_str());

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::startStream set event %s, %p\n", event, this);

		m_request->set_session(szSession);
		m_request->set_single_utterance(true);
		auto* queryInput = m_request->mutable_query_input();
		auto* audio_config = queryInput->mutable_audio_config();
		audio_config->set_sample_rate_hertz(16000);
		audio_config->set_audio_encoding(AudioEncoding::AUDIO_ENCODING_LINEAR_16);
		audio_config->set_language_code(m_lang.c_str());
		if (event) {
			auto* eventInput = queryInput->mutable_event();
			eventInput->set_name(event);
			eventInput->set_language_code(m_lang.c_str());
		}

  	m_streamer = m_stub->StreamingDetectIntent(m_context.get());
  	m_streamer->Write(*m_request);
	}
	bool write(void* data, uint32_t datalen) {
		if (m_finished) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::write not writing because we are finished, %p\n", this);
			return false;
		}

		m_request->clear_query_input();
		m_request->clear_query_params();
		m_request->set_input_audio(data, datalen);

		//switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::write wrote audio..%p\n", this);
		m_packets++;
    return m_streamer->Write(*m_request);

	}
	bool read(StreamingDetectIntentResponse* response) {
		return m_streamer->Read(response);
	}
	grpc::Status finish() {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "GStreamer::finish %p\n", this);
		if (m_finished) {
			grpc::Status ok;
			return ok;
		}
		m_finished = true;
		return m_streamer->Finish();
	}
	void writesDone() {
		m_streamer->WritesDone();
	}

private:
	std::string m_sessionId;
	std::shared_ptr<grpc::ClientContext> m_context;
	std::shared_ptr<grpc::ChannelCredentials> m_creds;
	std::shared_ptr<grpc::Channel> m_channel;
	std::unique_ptr<Sessions::Stub> 	m_stub;
	std::unique_ptr< grpc::ClientReaderWriterInterface<StreamingDetectIntentRequest, StreamingDetectIntentResponse> > m_streamer;
	std::shared_ptr<StreamingDetectIntentRequest> m_request;
	std::string m_lang;
	std::string m_projectId;
	bool m_finished;
	uint32_t m_packets;
};

static void killcb(struct cap_cb* cb) {
	if (cb) {
		if (cb->streamer) {
			GStreamer* p = (GStreamer *) cb->streamer;
			delete p;
			cb->streamer = NULL;
		}
		if (cb->resampler) {
				speex_resampler_destroy(cb->resampler);
				cb->resampler = NULL;
		}
	}
}

static void *SWITCH_THREAD_FUNC grpc_read_thread(switch_thread_t *thread, void *obj) {
	struct cap_cb *cb = (struct cap_cb *) obj;
	GStreamer* streamer = (GStreamer *) cb->streamer;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "grpc_read_thread: starting cb %p\n", (void *) cb);

	// Our contract: while we are reading, cb and cb->streamer will not be deleted

	// Read responses until there are no more
	StreamingDetectIntentResponse response;
	while (streamer->read(&response)) {  
		switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
		if (psession) {
			switch_channel_t* channel = switch_core_session_get_channel(psession);
			GRPCParser parser(psession);

			if (response.has_query_result() || response.has_recognition_result()) {
				cJSON* jResponse = parser.parse(response) ;
				char* json = cJSON_PrintUnformatted(jResponse);
				const char* type = DIALOGFLOW_EVENT_TRANSCRIPTION;

				if (response.has_query_result()) type = DIALOGFLOW_EVENT_INTENT;
				else {
					const StreamingRecognitionResult_MessageType& o = response.recognition_result().message_type();
					if (0 == StreamingRecognitionResult_MessageType_Name(o).compare("END_OF_SINGLE_UTTERANCE")) {
						type = DIALOGFLOW_EVENT_END_OF_UTTERANCE;
					}
				}

				cb->responseHandler(psession, type, json);

				free(json);
				cJSON_Delete(jResponse);
			}

			const std::string& audio = parser.parseAudio(response);
			bool playAudio = !audio.empty() ;

			// save audio
			if (playAudio) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "grpc_read_thread: received audio to play\n");

				// write audio to wave file
				switch_snprintf(cb->audioFile, sizeof(cb->audioFile), "%s%s%s_%d.tmp.wav", SWITCH_GLOBAL_dirs.temp_dir, 
					SWITCH_PATH_SEPARATOR, cb->sessionId, ++playCount);
				std::ofstream f(cb->audioFile, std::ofstream::binary);
				f << audio;
				f.close();
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "grpc_read_thread: wrote audio to %s\n", cb->audioFile);

				cJSON * jResponse = cJSON_CreateObject();
				cJSON_AddItemToObject(jResponse, "path", cJSON_CreateString(cb->audioFile));
				char* json = cJSON_PrintUnformatted(jResponse);

				cb->responseHandler(psession, DIALOGFLOW_EVENT_AUDIO_PROVIDED, json);
				free(json);
				cJSON_Delete(jResponse);
			}
			switch_core_session_rwunlock(psession);
		}
		else {
			break;
		}
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dialogflow read loop is done\n");

	// finish the detect intent session: here is where we may get an error if credentials are invalid
	switch_core_session_t* psession = switch_core_session_locate(cb->sessionId);
	if (psession) {
		grpc::Status status = streamer->finish();
		if (!status.ok()) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "StreamingDetectIntentRequest finished with err %s (%d): %s\n", 
				status.error_message().c_str(), status.error_code(), status.error_details().c_str());
			cb->errorHandler(psession, status.error_message().c_str());
		}
		cb->completionHandler(psession);

		switch_core_session_rwunlock(psession);
	}
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "dialogflow read thread exiting	\n");
}

extern "C" {
	switch_status_t google_dialogflow_init() {
		const char* gcsServiceKeyFile = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
		if (NULL == gcsServiceKeyFile) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, 
				"Error: \"GOOGLE_APPLICATION_CREDENTIALS\" environment variable must be set to path of the file containing service account json key\n");
			return SWITCH_STATUS_FALSE;     
		}
		return SWITCH_STATUS_SUCCESS;
	}
	
	switch_status_t google_dialogflow_cleanup() {
		return SWITCH_STATUS_SUCCESS;
	}

	// start dialogflow on a channel
	switch_status_t google_dialogflow_session_init(
		switch_core_session_t *session, 
		responseHandler_t responseHandler, 
		errorHandler_t errorHandler, 
		completionHandler_t completionHandler, 
		uint32_t samples_per_second, 
		char* lang, 
		char* projectId, 
		char* event, 
		struct cap_cb **ppUserData
	) {
		switch_status_t status = SWITCH_STATUS_SUCCESS;
		switch_channel_t *channel = switch_core_session_get_channel(session);
		int err;
		switch_threadattr_t *thd_attr = NULL;
		switch_memory_pool_t *pool = switch_core_session_get_pool(session);

		struct cap_cb* cb = (struct cap_cb *) switch_core_session_alloc(session, sizeof(*cb));
		cb->base = switch_core_session_strdup(session, "mod_dialogflow");
		strncpy(cb->sessionId, switch_core_session_get_uuid(session), 256);
		cb->responseHandler = responseHandler;
		cb->completionHandler = completionHandler;
		if (switch_mutex_init(&cb->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing mutex\n");
			status = SWITCH_STATUS_FALSE;
			goto done; 
		}

		strncpy(cb->lang, lang, MAX_LANG);
		strncpy(cb->projectId, lang, MAX_PROJECT_ID);
		cb->streamer = new GStreamer(session, lang, projectId, event);
		cb->resampler = speex_resampler_init(1, 8000, 16000, SWITCH_RESAMPLE_QUALITY, &err);
		if (0 != err) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error initializing resampler: %s.\n", 
						switch_channel_get_name(channel), speex_resampler_strerror(err));
			status = SWITCH_STATUS_FALSE;
			goto done;
		}

		// create the read thread
		switch_threadattr_create(&thd_attr, pool);
		switch_threadattr_detach_set(thd_attr, 1);
		switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
		switch_thread_create(&cb->thread, thd_attr, grpc_read_thread, cb, pool);

		*ppUserData = cb;
	
	done:
		if (status != SWITCH_STATUS_SUCCESS) {
			killcb(cb);
		}
		return status;
	}

	switch_status_t google_dialogflow_session_stop(switch_core_session_t *session) {
		switch_channel_t *channel = switch_core_session_get_channel(session);
		switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);

		if (bug) {
			struct cap_cb *cb = (struct cap_cb *) switch_core_media_bug_get_user_data(bug);
			switch_status_t st;

			// close connection and get final responses
			switch_mutex_lock(cb->mutex);
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer) {
				streamer->writesDone();
				streamer->finish();
			}

			killcb(cb);

			switch_channel_set_private(channel, MY_BUG_NAME, NULL);
			//switch_core_media_bug_destroy(&bug);
			switch_core_media_bug_close(&bug, SWITCH_FALSE);
			//switch_core_media_bug_remove_all_function(session, "dialogflow");
			switch_mutex_unlock(cb->mutex);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "google_dialogflow_session_cleanup: Closed google session\n");

			return SWITCH_STATUS_SUCCESS;
		}

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "%s Bug is not attached.\n", switch_channel_get_name(channel));
		return SWITCH_STATUS_FALSE;
	}
	
	switch_bool_t google_dialogflow_frame(switch_media_bug_t *bug, void* user_data) {
		switch_core_session_t *session = switch_core_media_bug_get_session(bug);
		uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
		switch_frame_t frame = {};
		struct cap_cb *cb = (struct cap_cb *) user_data;

		frame.data = data;
		frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

		if (switch_mutex_trylock(cb->mutex) == SWITCH_STATUS_SUCCESS) {
			GStreamer* streamer = (GStreamer *) cb->streamer;
			if (streamer) {
				while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS && !switch_test_flag((&frame), SFF_CNG)) {
					if (frame.datalen) {
						spx_int16_t out[SWITCH_RECOMMENDED_BUFFER_SIZE];
						spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE;
						spx_uint32_t in_len = frame.samples;
						size_t written;
						
						speex_resampler_process_interleaved_int(cb->resampler, (const spx_int16_t *) frame.data, (spx_uint32_t *) &in_len, &out[0], &out_len);
						
						streamer->write( &out[0], sizeof(spx_int16_t) * out_len);
					}
				}
			}
			else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
					"google_dialogflow_frame: not sending audio because google channel has been closed\n");
			}
			switch_mutex_unlock(cb->mutex);
		}
		else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
				"google_dialogflow_frame: not sending audio since failed to get lock on mutex\n");
		}
		return SWITCH_TRUE;
	}

	void destroyChannelUserData(struct cap_cb* cb) {
		killcb(cb);
	}

}