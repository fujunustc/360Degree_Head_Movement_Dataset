#include "VideoReader.hpp"
#include "Frame.hpp"

//#ifdef USE_OPENGL
#include <GL/glew.h>
#include <GL/gl.h>
//#endif

#include <iostream>
#include <Packet.hpp>
#include <stdexcept>

#define DEBUG_VideoReader 0
#if DEBUG_VideoReader
#define PRINT_DEBUG_VideoReader(s) std::cout << "DEC -- " << s << std::endl;
#else
#define PRINT_DEBUG_VideoReader(s) {}
#endif // DEBUG_VideoReader

extern "C"
{
   #include <libavutil/imgutils.h>
//   #include <libavutil/parseutils.h>
   #include <libswscale/swscale.h>
}

using namespace IMT::LibAv;

VideoReader::VideoReader(std::string inputPath): m_inputPath(inputPath), m_fmt_ctx(nullptr), m_videoStreamIds(),
    m_outputFrames(), m_streamIdToVecId(), m_nbFrames(0), m_doneVect(), m_gotOne(), m_startDisplayTime(std::chrono::system_clock::now()),
    m_swsCtx(nullptr), m_frame_ptr2(nullptr), m_decodingThread()
{
}

VideoReader::~VideoReader()
{
  if (m_decodingThread.joinable())
  {
    std::cout << "Join decoding thread\n";
    m_outputFrames.Stop();
    m_decodingThread.join();
    std::cout << "Join decoding thread: done\n";
  }
  if (m_fmt_ctx != nullptr)
  {
    for (unsigned i = 0; i < m_fmt_ctx->nb_streams; ++i)
    {
      avcodec_close(m_fmt_ctx->streams[i]->codec);
      //av_free(m_fmt_ctx->streams[i]->codec);
    }
    avformat_close_input(&m_fmt_ctx);
    avformat_free_context(m_fmt_ctx);
    m_fmt_ctx = nullptr;
  }
  if (m_frame_ptr2 != nullptr)
  {
    av_freep(&m_frame_ptr2->data[0]);
    av_frame_unref(m_frame_ptr2);
    av_frame_free(&m_frame_ptr2);
  }
}

void printA(AVFormatContext* _a)
{
std::cout << "duration    " << (unsigned long)_a->duration << "\n";
std::cout << "streams     " << _a->nb_streams << "\n";
unsigned nbVideo = 0;
for (unsigned i = 0; i < _a->nb_streams; ++i)
{
    if(_a->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        nbVideo++;
    }
}
std::cout << "vid stream  " << nbVideo << "\n";
std::cout << "format name " << _a->iformat->name << "\n";
std::cout << "bit_rate    " << _a->bit_rate << "\n";
std::cout << "long name   " << _a->iformat->long_name << "\n";
}

void VideoReader::Init(unsigned nbFrames)
{
    m_nbFrames = nbFrames;
    PRINT_DEBUG_VideoReader("Register codecs");
    av_register_all();

    int ret = 0;

//    uint8_t* buffer = nullptr;
//    size_t buffer_size;
//    int ret = av_file_map(m_inputPath.c_str(), &buffer, &buffer_size, 0, nullptr);
//    if (ret < 0)
//    {
//        std::cout << "Error while slurping the file content into the buffer" << std::endl;
//    }

//    struct buffer_data bd = { 0 };
//    bd.ptr  = buffer;
//    bd.size = buffer_size;

    PRINT_DEBUG_VideoReader("Allocate format context");
    if (!(m_fmt_ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        std::cout << "Error while allocating the format context" << std::endl;
    }

    PRINT_DEBUG_VideoReader("Open format context: "<<m_inputPath);
    ret = avformat_open_input(&m_fmt_ctx, m_inputPath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cout << "Could not open input" << std::endl;
    }

    PRINT_DEBUG_VideoReader("Find streams info");
    ret = avformat_find_stream_info(m_fmt_ctx, nullptr);
    if (ret < 0) {
        std::cout << "Could not find stream information" << std::endl;
    }

    PRINT_DEBUG_VideoReader("Dump format context");
    av_dump_format(m_fmt_ctx, 0, m_inputPath.c_str(), 0);

    printA(m_fmt_ctx);

    PRINT_DEBUG_VideoReader("Init video stream decoders");
    if (m_fmt_ctx->nb_streams > 1)
    {
      throw(std::invalid_argument("Support only video with one video stream"));
    }
    for (unsigned i = 0; i < m_fmt_ctx->nb_streams; ++i)
    {
        if(m_fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            m_fmt_ctx->streams[i]->codec->refcounted_frames = 1;
            //m_outputFrames.emplace_back();
            m_streamIdToVecId[i] = m_videoStreamIds.size();
            m_videoStreamIds.push_back(i);
            auto* decoder = avcodec_find_decoder(m_fmt_ctx->streams[i]->codec->codec_id);
            if(!decoder)
            {
                std::cout << "Could not find the decoder for stream id " << i << std::endl;
            }
            PRINT_DEBUG_VideoReader("Init decoder for stream id " << i);
            if ((ret = avcodec_open2(m_fmt_ctx->streams[i]->codec, decoder, nullptr)) < 0)
            {
                std::cout << "Could not open the decoder for stream id " << i << std::endl;
            }
        }
    }
    m_outputFrames.SetTotal(nbFrames);
    m_doneVect = std::vector<bool>(1, false);
    m_gotOne = std::vector<bool>(1, false);

    std::cout << "Start decoding thread\n";
    m_decodingThread = std::thread(&VideoReader::RunDecoderThread, this);
}

// static bool AllDone(const std::vector<bool>& vect)
// {
//     bool r = true;
//     for (auto b: vect)
//     {
//         r &= b;
//     }
//     return r;
// }

// static std::shared_ptr<cv::Mat> ToMat(AVCodecContext* codecCtx, AVFrame* frame_ptr)
// {
//     int w = codecCtx->width;
//     int h = codecCtx->height;
//     struct SwsContext* convert_ctx;
//     convert_ctx = sws_getContext(w, h, codecCtx->pix_fmt, w, h, AV_PIX_FMT_BGR24, SWS_FAST_BILINEAR,
//         NULL, NULL, NULL);
//     if(convert_ctx == NULL)
//     {
//         std::cout << "Cannot initialize the conversion context!" << std::endl;
//     }
//     AVFrame* frame_ptr2 = av_frame_alloc();
//     av_image_alloc(frame_ptr2->data, frame_ptr2->linesize, w, h, AV_PIX_FMT_BGR24, 1);
//     sws_scale(convert_ctx, frame_ptr->data, frame_ptr->linesize, 0, h, frame_ptr2->data, frame_ptr2->linesize);
//     cv::Mat mat(codecCtx->height, codecCtx->width, CV_8UC3, frame_ptr2->data[0], frame_ptr2->linesize[0]);
//
//     auto returnMat = std::make_shared<cv::Mat>(mat.clone());
//     av_freep(&frame_ptr2->data[0]);
//     av_frame_unref(frame_ptr2);
//     av_frame_free(&frame_ptr2);
//     mat.release();
//     sws_freeContext(convert_ctx);
//     return returnMat;
// }

void VideoReader::RunDecoderThread(void)
{
    AVPacket pkt;
    int ret = -1;
    PRINT_DEBUG_VideoReader("Read next pkt")
    while ((ret = av_read_frame(m_fmt_ctx, &pkt)) >= 0)
    {
        unsigned streamId = pkt.stream_index;
        if (m_streamIdToVecId.count(streamId) > 0 && !m_doneVect[m_streamIdToVecId[streamId]])
        { //then the pkt belong to a stream we care about.
            PRINT_DEBUG_VideoReader("Got a pkt for streamId "<<streamId)
            const AVPacket* const packet_ptr = &pkt;
            bool got_a_frame = false;
            auto* codecCtx = m_fmt_ctx->streams[streamId]->codec;
            PRINT_DEBUG_VideoReader("Send the packet to decoder for streamId "<<streamId)
            ret = avcodec_send_packet(codecCtx, packet_ptr);
            if (ret == 0)
            {//success to send packet
                while(true)
                {
                  PRINT_DEBUG_VideoReader("Ask if a frame is available for streamId " <<streamId)
                  auto frame = std::make_shared<Frame>();
                  ret = frame->AvCodecReceiveFrame(codecCtx);
                  got_a_frame = got_a_frame || (ret == 0);
                  if (ret == 0)
                  {
                      PRINT_DEBUG_VideoReader("Got a frame for streamId " <<streamId)
                      m_gotOne[m_streamIdToVecId[streamId]] = true;
                      //m_outputFrames[m_streamIdToVecId[streamId]].push(std::move(frame));
                      if (!m_outputFrames.Add(std::move(frame)))
                      {
                        std::cout << "Decoding thread stopped: frame limite exceed\n";
                        return;
                      }
                  }
                  else
                  {
                    PRINT_DEBUG_VideoReader("No frame available for streamId " <<streamId)
                    break;
                  }
                }
            }
            //m_doneVect[m_streamIdToVecId[streamId]] = (m_gotOne[m_streamIdToVecId[streamId]] && (!got_a_frame)) || (m_outputFrames[m_streamIdToVecId[streamId]].size() >= m_nbFrames);
        }
        av_packet_unref(&pkt);
    }
    { //flush all the rest
        PRINT_DEBUG_VideoReader("Start to flush")
        unsigned streamVectId = 0;
        for (auto i = 0; i < 1; ++i)
        {
          //send flush signal
          PRINT_DEBUG_VideoReader("Send flush signal for streamId "<<i)
          avcodec_send_packet(m_fmt_ctx->streams[m_videoStreamIds[i]]->codec, nullptr);
        }
        while(true)//we decode all the reste of the frames
        {
            if (!m_doneVect[streamVectId])
            {

                bool got_a_frame = false;
                auto* codecCtx = m_fmt_ctx->streams[m_videoStreamIds[streamVectId]]->codec;
                PRINT_DEBUG_VideoReader("Ask for next frame for streamVectId "<<streamVectId)
                auto frame = std::make_shared<Frame>();
                int ret = frame->AvCodecReceiveFrame(m_fmt_ctx->streams[m_videoStreamIds[streamVectId]]->codec);
                got_a_frame = ret == 0;
                if (got_a_frame)
                {
                    PRINT_DEBUG_VideoReader("Got a frame for streamVectId "<<streamVectId)
                    if(!m_outputFrames.Add(std::move(frame)))
                    {
                      std::cout << "Decoding thread stopped: frame limite exceed\n";
                      return;
                    }
                    //m_outputFrames[streamVectId].emplace();
                }
                else
                {
                  std::cout << "Decoding thread stopped: video done\n";
                  m_outputFrames.SetTotal(0);
                  return;
                }
                //m_doneVect[streamVectId] = (!got_a_frame) || (m_outputFrames[streamVectId].size() >= m_nbFrames);
                //streamVectId = (streamVectId + 1) % m_outputFrames.size();
            }
        }
    }
}

//#ifdef USE_OPENGL
void VideoReader::SetNextPictureToOpenGLTexture(unsigned streamId, std::chrono::system_clock::time_point deadline)
{
    auto now = std::chrono::system_clock::now();
    bool first = (m_swsCtx == nullptr);
    if (first)
    {
      m_startDisplayTime = now;
    }
    auto timeSinceStartDisplay = now - m_startDisplayTime;
    if (streamId < 1)
    {
        if (!m_outputFrames.IsAllDones())
        {
            PRINT_DEBUG_VideoReader("Forward next picture for streamId "<<streamId)
            // auto matPtr = m_outputFrames[streamId].front();
            // m_outputFrames[streamId].pop();
            // return matPtr;
            std::shared_ptr<Frame> frame(nullptr);
            bool done = false;
            size_t nbUsed = 0;
            while(!done)
            {
              auto tmp_frame = m_outputFrames.Get();
              if (tmp_frame != nullptr)
              {
                if (timeSinceStartDisplay >= std::chrono::milliseconds(tmp_frame->GetDisplayTimestamp()))
                {
                  frame = std::move(tmp_frame);
                  m_outputFrames.Pop();
                  ++nbUsed;
                }
                else
                {
                  done = true;
                }
              }
              else
              {
                std::cout << "No more frames \n";
                done = true;
              }

            }

            if (nbUsed > 1)
            {
              std::cout << "Nb dropped frame: " << nbUsed-1 << std::endl;
            }
            if (frame != nullptr && frame->IsValid())
            {
              auto w = frame->GetWidth();
              auto h = frame->GetHeight();
              if (m_swsCtx == nullptr)
              {
                m_swsCtx = sws_getContext(w, h,
                                  AV_PIX_FMT_YUV420P, w, h,
                                  AV_PIX_FMT_RGB24, 0, 0, 0, 0);
                m_frame_ptr2 = av_frame_alloc();
                av_image_alloc(m_frame_ptr2->data, m_frame_ptr2->linesize, w, h, AV_PIX_FMT_BGR24, 1);
              }
              //transform from YUV420p domain to RGB domain
              sws_scale(m_swsCtx, frame->GetDataPtr(), frame->GetRowLength(), 0, h, m_frame_ptr2->data, m_frame_ptr2->linesize);

              if (first)
              {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, m_frame_ptr2->data[0]);
              }
              else
              {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, m_frame_ptr2->data[0]);
              }


              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
              glGenerateMipmap(GL_TEXTURE_2D);
            }
        }
    }
    return;
}
//#endif
