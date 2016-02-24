/*
 *      Copyright (C) 2016-2016 peak3d
 *      http://www.peak3d.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <iostream>
#include <string.h>
#include <sstream>

#include "xbmc_addon_types.h"
#include "libXBMC_addon.h"
#include "kodi_inputstream_types.h"

#include "Ap4.h"

#define SAFE_DELETE(p)       do { delete (p);     (p)=NULL; } while (0)

ADDON::CHelper_libXBMC_addon *xbmc = 0;

/*******************************************************
|   FragmentedSampleReader
********************************************************/
class FragmentedSampleReader : public AP4_LinearReader
{
public:

  FragmentedSampleReader(AP4_ByteStream *input, AP4_Movie *movie, AP4_Track *track, 
    AP4_UI32 streamId, AP4_UI08 nls)
    : AP4_LinearReader(*movie, input)
    , m_Track(track)
    , m_dts(0.0)
    , m_pts(0.0)
    , m_pictureId(0)
    , m_lastPictureId(0)
    , m_eos(false)
    , m_StreamId(streamId)
    , m_NaluLengthSize(nls)
  {
    EnableTrack(m_Track->GetId());
  }

  ~FragmentedSampleReader()
  {
  }

  AP4_Result ReadSample()
  {
    AP4_Result result;
    if (AP4_FAILED(result = ReadNextSample(m_Track->GetId(), m_sample_, m_sample_data_)))
    {
      if (result == AP4_ERROR_EOS) {
        m_eos = true;
      }
      else {
        return result;
      }
    }
    m_dts = (double)m_sample_.GetDts() / (double)m_Track->GetMediaTimeScale();
    m_pts = (double)m_sample_.GetCts() / (double)m_Track->GetMediaTimeScale();
    
    //Search the Slice header NALU
    const AP4_UI08 *data(m_sample_data_.GetData());
    unsigned int data_size(m_sample_data_.GetDataSize());
    for (;data_size;)
    {
      // sanity check
      if (data_size < m_NaluLengthSize)
        break;

      // get the next NAL unit
      AP4_UI32 nalu_size;
      switch (m_NaluLengthSize) {
        case 1:nalu_size = *data++; data_size--; break;
        case 2:nalu_size = AP4_BytesToInt16BE(data); data += 2; data_size -= 2; break;
        case 4:nalu_size = AP4_BytesToInt32BE(data); data += 4; data_size -= 4; break;
        default: data_size = 0; nalu_size = 1; break;
      }
      if (nalu_size > data_size)
        break;

      unsigned int nal_unit_type = *data & 0x1F;

      if (nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_OF_NON_IDR_PICTURE ||
        nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_OF_IDR_PICTURE ||
        nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_A ||
        nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_B ||
        nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_C) {

        AP4_DataBuffer unescaped(data, data_size);
        AP4_NalParser::Unescape(unescaped);
        AP4_BitReader bits(unescaped.GetData(), unescaped.GetDataSize());

        bits.SkipBits(8); // NAL Unit Type

        AP4_AvcFrameParser::ReadGolomb(bits); // first_mb_in_slice
        AP4_AvcFrameParser::ReadGolomb(bits); // slice_type
        m_pictureId = AP4_AvcFrameParser::ReadGolomb(bits);
        break;
      }
      // move to the next NAL unit
      data += nalu_size;
      data_size -= nalu_size;
    }
    return AP4_SUCCESS;
  };

  bool GetVideoInformation(unsigned int &width, unsigned int &height)
  {
    if (m_pictureId != m_lastPictureId)
    {
      AP4_SampleDescription *desc = m_Track->GetSampleDescription(0);
      if (desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED)
        desc = static_cast<AP4_ProtectedSampleDescription*>(desc)->GetOriginalSampleDescription();
      if (AP4_AvcSampleDescription *avc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, desc))
      {
        AP4_Array<AP4_DataBuffer>& buffer = avc->GetPictureParameters();
        AP4_AvcPictureParameterSet pps;
        for (unsigned int i(0); i < buffer.ItemCount(); ++i)
        {
          if (AP4_SUCCEEDED(AP4_AvcFrameParser::ParsePPS(buffer[i].GetData(), buffer[i].GetDataSize(), pps)) && pps.pic_parameter_set_id == m_pictureId)
          {
            buffer = avc->GetSequenceParameters();
            AP4_AvcSequenceParameterSet sps;
            for (unsigned int i(0); i < buffer.ItemCount(); ++i)
            {
              if (AP4_SUCCEEDED(AP4_AvcFrameParser::ParseSPS(buffer[i].GetData(), buffer[i].GetDataSize(), sps)) && sps.seq_parameter_set_id == pps.seq_parameter_set_id)
              {
                sps.GetInfo(width, height);
                break;
              }
            }
            break;
          }
        }
      }
      m_lastPictureId = m_pictureId;
      return true;
    }
    return false;
  }

  bool EOS()const{ return m_eos; };
  void SetEOS(bool eos){ m_eos = eos; };
  double DTS()const{ return m_dts; };
  double PTS()const{ return m_pts; };
  const AP4_Sample &Sample()const { return m_sample_; };
  AP4_UI32 GetStreamId()const{ return m_StreamId; };
  AP4_Size GetSampleDataSize()const{ return m_sample_data_.GetDataSize(); };
  const AP4_Byte *GetSampleData()const{ return m_sample_data_.GetData(); };
  double GetDuration()const{ return (double)m_sample_.GetDuration() / (double)m_Track->GetMediaTimeScale(); };

private:
  AP4_Track *m_Track;
  AP4_UI32 m_StreamId;
  bool m_eos;
  double m_dts, m_pts;
  AP4_UI08 m_NaluLengthSize;
  uint8_t m_pictureId, m_lastPictureId;

  AP4_Sample     m_sample_;
  AP4_DataBuffer m_sample_data_;
};

/*******************************************************
Main class Session
********************************************************/
class Session
{
public:
  Session();
  ~Session();
  bool initialize();
  void SetStreamProperties(uint16_t width, uint16_t height, const char* language, uint32_t maxBitPS, bool allow_ec_3);
  FragmentedSampleReader *GetNextSample();
  INPUTSTREAM_INFO *GetStreamInfo(unsigned int sid){ return sid == 1 ? &video_info_ : sid == 2 ? &audio_info_ : 0; };
private:
  AP4_ByteStream *video_input_, *audio_input_;
  AP4_File *video_input_file_, *audio_input_file_;
  INPUTSTREAM_INFO video_info_, audio_info_;

  uint16_t width_, height_;
  std::string language_;
  uint32_t fixed_bandwidth_;

  FragmentedSampleReader *audio_reader_, *video_reader_;
} *session = 0;

Session::Session()
  : video_input_(NULL)
  , audio_input_(NULL)
  , video_input_file_(NULL)
  , audio_input_file_(NULL)
  , audio_reader_(NULL)
  , video_reader_(NULL)
{
  memset(&audio_info_, 0, sizeof(audio_info_));
  memset(&video_info_, 0, sizeof(video_info_));

  audio_info_.m_streamType = INPUTSTREAM_INFO::TYPE_AUDIO;
  audio_info_.m_pID = 1;

  video_info_.m_streamType = INPUTSTREAM_INFO::TYPE_VIDEO;
  video_info_.m_pID = 2;
}

Session::~Session()
{
  delete video_reader_;
  delete video_input_;

  delete audio_reader_;
  delete audio_input_;
}

/*----------------------------------------------------------------------
|   initialize
+---------------------------------------------------------------------*/

static bool copyLang(char* dest, const char* src)
{
  size_t len(strlen(src));

  if (len && len != 3)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Invalid language in trak atom (%s)", src);
    return false;
  }
  strcpy(dest, src);
  return true;
}

bool Session::initialize()
{
  AP4_Result result;
  /************ VIDEO INITIALIZATION ******/
  result = AP4_FileByteStream::Create("C:\\Temp\\video.mov", AP4_FileByteStream::STREAM_MODE_READ, video_input_);
  if (AP4_FAILED(result)) {
    xbmc->Log(ADDON::LOG_ERROR, "Cannot open video.mov!");
    return false;
  }
  video_input_file_ = new AP4_File(*video_input_, AP4_DefaultAtomFactory::Instance, true);
  AP4_Movie* movie = video_input_file_->GetMovie();
  if (movie == NULL)
  {
    xbmc->Log(ADDON::LOG_ERROR, "No MOOV in video stream!");
    return false;
  }
  AP4_Track *track = movie->GetTrack(AP4_Track::TYPE_VIDEO);
  if (!track)
  {
    xbmc->Log(ADDON::LOG_ERROR, "No suitable track found in video stream");
    return false;
  }

  video_info_.m_ExtraSize = 0;
  video_info_.m_ExtraData = 0;
  
  AP4_SampleDescription *desc = track->GetSampleDescription(0);
  if (desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED)
    desc = static_cast<AP4_ProtectedSampleDescription*>(desc)->GetOriginalSampleDescription();
  AP4_VideoSampleDescription *video_sample_description = AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, desc);
  if (video_sample_description == NULL)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Unable to parse video sample description!");
    return false;
  }
  
  AP4_UI08 naluLengthSize = 0;

  video_info_.m_Width = video_sample_description->GetWidth();
  video_info_.m_Height = video_sample_description->GetHeight();
  video_info_.m_Aspect = 1.0;

  switch (desc->GetFormat())
  {
  case AP4_SAMPLE_FORMAT_AVC1:
  case AP4_SAMPLE_FORMAT_AVC2:
  case AP4_SAMPLE_FORMAT_AVC3:
  case AP4_SAMPLE_FORMAT_AVC4:
    strcpy(video_info_.m_codecName, "h264");
    if (AP4_AvcSampleDescription *avc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, desc))
    {
      video_info_.m_ExtraSize = avc->GetRawBytes().GetDataSize();
      video_info_.m_ExtraData = avc->GetRawBytes().GetData();
      if (avc->GetPictureParameters().ItemCount() > 1 || !video_info_.m_Width || !video_info_.m_Height)
        naluLengthSize = avc->GetNaluLengthSize();
    }
    break;
  case AP4_SAMPLE_FORMAT_HEV1:
  case AP4_SAMPLE_FORMAT_HVC1:
    strcpy(video_info_.m_codecName, "hevc");
    if (AP4_HevcSampleDescription *hevc = AP4_DYNAMIC_CAST(AP4_HevcSampleDescription, desc))
    {
      video_info_.m_ExtraSize = hevc->GetRawBytes().GetDataSize();
      video_info_.m_ExtraData = hevc->GetRawBytes().GetData();
      //naluLengthSize = hevc->GetNaluLengthSize();
    }
    break;
  default:
    xbmc->Log(ADDON::LOG_ERROR, "Video codec not supported");
    return false;
  }

  video_reader_ = new FragmentedSampleReader(video_input_, movie, track, 1, naluLengthSize);

  if (!AP4_SUCCEEDED(video_reader_->ReadSample()))
    return false;

  video_reader_->GetVideoInformation(video_info_.m_Width, video_info_.m_Height);

  /************ AUDIO INITIALIZATION ******/
  result = AP4_FileByteStream::Create("C:\\Temp\\audio.mov", AP4_FileByteStream::STREAM_MODE_READ, audio_input_);
  if (AP4_FAILED(result)) {
    xbmc->Log(ADDON::LOG_ERROR, "Cannot open audio.mov!");
    return false;
  }
  audio_input_file_ = new AP4_File(*audio_input_, AP4_DefaultAtomFactory::Instance, true);
  movie = audio_input_file_->GetMovie();
  if (movie == NULL)
  {
    xbmc->Log(ADDON::LOG_ERROR, "No MOOV in audio stream!");
    return false;
  }
  track = movie->GetTrack(AP4_Track::TYPE_AUDIO);
  if (!track)
  {
    xbmc->Log(ADDON::LOG_ERROR, "No suitable track found in audio stream!");
    return false;
  }

  if (!copyLang(audio_info_.m_language, track->GetTrackLanguage()))
    return false;

  desc = track->GetSampleDescription(0);
  if (desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED)
    desc = static_cast<AP4_ProtectedSampleDescription*>(desc)->GetOriginalSampleDescription();
  AP4_AudioSampleDescription *audio_sample_description = AP4_DYNAMIC_CAST(AP4_AudioSampleDescription, desc);
  if (audio_sample_description == NULL)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Unable to parse audio sample description!");
    return false;
  }
  switch (desc->GetFormat())
  {
  case AP4_SAMPLE_FORMAT_MP4A:
    strcpy(audio_info_.m_codecName, "aac");
    if (AP4_MpegSampleDescription *aac = AP4_DYNAMIC_CAST(AP4_MpegSampleDescription, desc))
    {
      audio_info_.m_ExtraSize = aac->GetDecoderInfo().GetDataSize();
      audio_info_.m_ExtraData = aac->GetDecoderInfo().GetData();
    }
    break;
  case  AP4_SAMPLE_FORMAT_AC_3:
  case AP4_SAMPLE_FORMAT_EC_3:
    strcpy(audio_info_.m_codecName, "eac3");
    break;
  default:
    xbmc->Log(ADDON::LOG_ERROR, "Audio codec not supported!");
    return false;
  }
  if (AP4_MpegSystemSampleDescription *esds = AP4_DYNAMIC_CAST(AP4_MpegSystemSampleDescription, audio_sample_description))
    audio_info_.m_BitRate = esds->GetAvgBitrate();

  audio_info_.m_BitsPerSample = audio_sample_description->GetSampleSize();
  audio_info_.m_Channels = audio_sample_description->GetChannelCount();
  audio_info_.m_SampleRate = audio_sample_description->GetSampleRate();
  audio_info_.m_ExtraSize = 0;
  audio_info_.m_ExtraData = 0;

  audio_reader_ = new FragmentedSampleReader(audio_input_, movie, track, 2, 0);

  if (!AP4_SUCCEEDED(audio_reader_->ReadSample()))
    return false;

  audio_reader_->SetEOS(true);

  return true;
}

FragmentedSampleReader *Session::GetNextSample()
{
  FragmentedSampleReader *stack[2];
  unsigned int numReader(0);

  if (!video_reader_->EOS())
    stack[numReader++] = video_reader_;
  if (!audio_reader_->EOS())
    stack[numReader++] = audio_reader_;

  FragmentedSampleReader *res(0);

  while (numReader--)
    if (!res || stack[numReader]->DTS() < res->DTS())
      res = stack[numReader];

  return res;
}

/***************************  Interface *********************************/

#include "kodi_inputstream_dll.h"
#include "libKODI_inputstream.h"

extern "C" {
  
  ADDON_STATUS curAddonStatus = ADDON_STATUS_UNKNOWN;
  CHelper_libKODI_inputstream *ipsh = 0;

  /***********************************************************
  * Standart AddOn related public library functions
  ***********************************************************/

  ADDON_STATUS ADDON_Create(void* hdl, void* props)
  {
    // initialize globals
    session = nullptr;

    if (!hdl)
      return ADDON_STATUS_UNKNOWN;

    xbmc = new ADDON::CHelper_libXBMC_addon;
    if (!xbmc->RegisterMe(hdl))
    {
      SAFE_DELETE(xbmc);
      return ADDON_STATUS_PERMANENT_FAILURE;
    }

    ipsh = new CHelper_libKODI_inputstream;
    if (!ipsh->RegisterMe(hdl))
    {
      SAFE_DELETE(xbmc);
      SAFE_DELETE(ipsh);
      return ADDON_STATUS_PERMANENT_FAILURE;
    }

    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: ADDON_Create()");

    curAddonStatus = ADDON_STATUS_UNKNOWN;

    //if (XBMC->GetSetting("host", buffer))

    curAddonStatus = ADDON_STATUS_OK;
    return curAddonStatus;
  }

  ADDON_STATUS ADDON_GetStatus()
  {
    return curAddonStatus;
  }

  void ADDON_Destroy()
  {
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: ADDON_Destroy()");
    SAFE_DELETE(session);
    SAFE_DELETE(xbmc);
    SAFE_DELETE(ipsh);
  }

  bool ADDON_HasSettings()
  {
    return false;
  }

  unsigned int ADDON_GetSettings(ADDON_StructSetting ***sSet)
  {
    return 0;
  }

  ADDON_STATUS ADDON_SetSetting(const char *settingName, const void *settingValue)
  {
    return ADDON_STATUS_OK;
  }

  void ADDON_Stop()
  {
  }

  void ADDON_FreeSettings()
  {
  }

  void ADDON_Announce(const char *flag, const char *sender, const char *message, const void *data)
  {
  }

  /***********************************************************
  * InputSteam Client AddOn specific public library functions
  ***********************************************************/

  bool Open(INPUTSTREAM& props)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: Open()");

    session = new Session();
    if (!session->initialize())
    {
      SAFE_DELETE(session);
      return false;
    }
    return true;
  }

  void Close(void)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: Close()");
    SAFE_DELETE(session);
  }

  const char* GetPathList(void)
  {
    static char buffer[1024];

    if (!xbmc->GetSetting("URL1", buffer))
      buffer[0] = 0;

    return buffer;
  }

  struct INPUTSTREAM_IDS GetStreamIds()
  {
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: GetStreamIds()");
    INPUTSTREAM_IDS iids;
    if (session)
    {
      iids.m_streamCount = 1;
      iids.m_streamIds[0] = 1;
      iids.m_streamIds[1] = 2;
    } else
      iids.m_streamCount = 0;

    return iids;
  }

  struct INPUTSTREAM_CAPABILITIES GetCapabilities()
  {
    INPUTSTREAM_CAPABILITIES caps;
    caps.m_supportsIDemux = true;
    caps.m_supportsIPosTime = false;
    caps.m_supportsIDisplayTime = true;
    caps.m_supportsSeek = true;
    caps.m_supportsPause = true;
    return caps;
  }

  struct INPUTSTREAM_INFO GetStream(int streamid)
  {
    static struct INPUTSTREAM_INFO dummy_info = {
      INPUTSTREAM_INFO::TYPE_NONE, "", 0, 0, 0, "",
      0, 0, 0, 0, 0.0f,
      0, 0, 0, 0, 0 };
    
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: GetStream(%d)", streamid);

    if (session)
    {
      INPUTSTREAM_INFO *info(session->GetStreamInfo(streamid));
      if (info)
        return *info;
    }
    return dummy_info;
  }

  void EnableStream(int streamid, bool enable)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "InputStream.mpd: EnableStream(%d, %d)", streamid, (int)enable);
  }

  int ReadStream(unsigned char*, unsigned int)
  {
    return -1;
  }

  long long SeekStream(long long, int)
  {
    return -1;
  }

  long long PositionStream(void)
  {
    return -1;
  }

  long long LengthStream(void)
  {
    return -1;
  }

  void DemuxReset(void)
  {
  }

  void DemuxAbort(void)
  {
  }

  void DemuxFlush(void)
  {
  }

  DemuxPacket* __cdecl DemuxRead(void)
  {
    if (!session)
      return NULL;

    FragmentedSampleReader *sr(session->GetNextSample());

    if (sr)
    {
      const AP4_Sample &s(sr->Sample());
      DemuxPacket *p = ipsh->AllocateDemuxPacket(sr->GetSampleDataSize());
      p->dts = sr->DTS() * 1000000;
      p->pts = sr->PTS() * 1000000;
      p->duration = sr->GetDuration() * 1000000;
      p->iStreamId = sr->GetStreamId();
      p->iGroupId = 0;
      p->iSize = sr->GetSampleDataSize();
      memcpy(p->pData, sr->GetSampleData(), p->iSize);

      xbmc->Log(ADDON::LOG_DEBUG, "DTS: %0.4f PTS: %0.4f ID: %u", p->dts, p->pts, p->iStreamId);

      sr->ReadSample();
      return p;
    }
    return NULL;
  }

  bool DemuxSeekTime(int time, bool backwards, double *startpts)
  {
    return false;
  }

  void DemuxSetSpeed(int speed)
  {

  }

  int GetTotalTime()
  {
    return 20;
  }

  int GetTime()
  {
    return 0;
  }

  bool CanPauseStream(void)
  {
    return true;
  }

  void PauseStream(double)
  {
  }

  bool CanSeekStream(void)
  {
    return false;
  }

  bool PosTime(int)
  {
    return false;
  }

  void SetSpeed(int)
  {
  }

  bool IsRealTimeStream(void)
  {
    return false;
  }

}//extern "C"
