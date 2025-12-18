#pragma once
#include <obs-module.h>

#include <chrono>
#include <string>
#include "logger.h"

class MoQOutput
{
      public:
    MoQOutput(obs_data_t *settings, obs_output_t *output);
    ~MoQOutput();

    bool Start();
    void Stop(bool signal = true);
    void Data(struct encoder_packet *packet);

    inline size_t GetTotalBytes()
    {
        return total_bytes_sent;
    }

    inline int GetConnectTime()
    {
        return connect_time_ms;
    }

      private:
    void VideoInit();
    void VideoData(struct encoder_packet *packet);
    void AudioInit();
    void AudioData(struct encoder_packet *packet);

    obs_output_t *output;

    std::string server_url;
    std::string path;

    size_t total_bytes_sent;
    int connect_time_ms;
    std::chrono::steady_clock::time_point connect_start;

    int origin;
    int session;
    int broadcast;
    int video;
    int audio;
};

void register_moq_output();
