#pragma once
#include <obs-module.h>

#include <string>
#include "logger.h"

class MoQOutput {
public:
	MoQOutput(obs_data_t *settings, obs_output_t *output);
	~MoQOutput();

	bool Start();
	void Stop(bool signal = true);
	void Data(struct encoder_packet *packet);

	inline size_t GetTotalBytes() { return total_bytes_sent; }

	inline int GetConnectTime() { return connect_time_ms; }

private:
	// TODO: Add needed functions
	void ConfigureVideoTrack();
	void ConfigureAudioTrack();

	obs_output_t *output;

	std::string server_url;
	std::string path;

	size_t total_bytes_sent;
	int connect_time_ms;
};

void register_moq_output();
