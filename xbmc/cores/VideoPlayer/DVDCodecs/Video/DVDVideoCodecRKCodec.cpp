/*
 *      Copyright (C) 2013 Team XBMC
 *      http://xbmc.org
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
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "DVDVideoCodecRKCodec.h"

#include "Application.h"
#include "messaging/ApplicationMessenger.h"
#include "DVDClock.h"
#include "utils/BitstreamConverter.h"
#include "utils/CPUInfo.h"
#include "utils/log.h"
#include "settings/AdvancedSettings.h"
///#include "platform/android/activity/XBMCApp.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"

///#include "platform/android/jni/ByteBuffer.h"
///#include "platform/android/jni/MediaCodec.h"
///#include "platform/android/jni/MediaCrypto.h"
///#include "platform/android/jni/MediaFormat.h"
///#include "platform/android/jni/MediaCodecList.h"
///#include "platform/android/jni/MediaCodecInfo.h"
///#include "platform/android/jni/Surface.h"
///#include "platform/android/jni/SurfaceTexture.h"
///#include "platform/android/activity/AndroidFeatures.h"
#include "settings/Settings.h"

#include <cassert>

#define CHECKEGL() { int e=glGetError(); if (e) \
        CLog::Log(LOGERROR, "%s#%d - EGL ERROR %d(0x%x)", \
        __FUNCTION__, __LINE__, e, e);}

using namespace KODI::MESSAGING;

enum MEDIACODEC_STATES
{
  MEDIACODEC_STATE_UNINITIALIZED,
  MEDIACODEC_STATE_CONFIGURED,
  MEDIACODEC_STATE_FLUSHED,
  MEDIACODEC_STATE_RUNNING,
  MEDIACODEC_STATE_ENDOFSTREAM,
  MEDIACODEC_STATE_ERROR
};

struct VpuCodecContext* vpu_ctx = NULL;
RK_S32 fileSize =0, pkt_size =0;
RK_S32 ret = 0, frame_count=0;
DecoderOut_t    decOut;
VideoPacket_t access_unit;
VideoPacket_t* pkt =NULL;
DecoderOut_t *pOut = NULL;
VPU_FRAME *frame = NULL;
RK_S64 fakeTimeUs =0;
RK_U8* pExtra = NULL;
RK_U32 extraSize = 0;
int extraSent = 0;
DecoderFormat_t fmt;

EGLDisplay m_eglDisplay = 0;
EGLSurface m_eglSurface = 0;
EGLContext m_eglContext = 0;
EGLConfig  m_eglConfig = 0;
EGLSurface m_eglTexSurface = 0;
EGLContext m_eglTexContext = 0;
EGLConfig  m_eglTexConfig = 0;
Display   *m_Display = 0;

typedef struct vpu_buf_t {
    // mem part
    VPUMemLinear_t *vpu_mem;
    int size;
    int dma_fd;
    // frame info
    OMX_RK_VIDEO_CODINGTYPE videoformat;
    int frameformat;
    int width;
    int height;
    int aligned_width;
    int aligned_height;
} vpu_buf_t;

#define N_VPUBUFS 32 

vpu_display_mem_pool* vpu_display_pool=NULL;
int vpu_display_pool_count = 0;
vpu_buf_t* mems[N_VPUBUFS];
std::queue<vpu_buf_t*> free_queue;

int CDVDVideoCodecRKCodec::s_instances = 0;

void createTexContext()
{
  if(m_eglTexConfig) return;

  CHECKEGL();
  eglBindAPI(EGL_OPENGL_ES_API);
  CHECKEGL();
  static const EGLint contextAttrs [] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

  m_eglTexConfig = m_eglConfig; // use main eglConfig
  m_eglTexContext = eglCreateContext(m_eglDisplay, m_eglTexConfig, m_eglContext, contextAttrs);
  CHECKEGL();
  if (m_eglTexContext == EGL_NO_CONTEXT)
  {
    CLog::Log(LOGERROR, "%s: Could not create a context",__FUNCTION__);
    return;
  }
  m_eglTexSurface = eglCreatePbufferSurface(m_eglDisplay, m_eglTexConfig, NULL);
  CHECKEGL();
  if (m_eglTexSurface == EGL_NO_SURFACE)
  {
    CLog::Log(LOGERROR, "%s: Could not create a surface",__FUNCTION__);
    return;
  }
  bool s = eglMakeCurrent(m_eglDisplay, m_eglTexSurface, m_eglTexSurface, m_eglTexContext);
  CHECKEGL();
  if (!s)
  {
    CLog::Log(LOGERROR, "%s: Could not make current",__FUNCTION__);
    return;
  }
}

void makeCurrent(bool withSurface)
{
  m_Display = g_Windowing.GetDisplay();
  m_eglDisplay = g_Windowing.GetEGLDisplay();
  m_eglContext = g_Windowing.GetEGLContext();
  m_eglConfig = g_Windowing.GetEGLConfig();
  m_eglSurface = g_Windowing.GetEGLSurface();

  bool ret = false;
  if(withSurface) {
    if (g_application.IsCurrentThread())
      ret = eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext);
    else {
      if(!m_eglTexContext)
	createTexContext();
      ret = eglMakeCurrent(m_eglDisplay, m_eglTexSurface, m_eglTexSurface, m_eglTexContext);
    }
  } else {
    ret = eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);
  }
  CHECKEGL();

  ///ret = eglMakeCurrent(m_eglDisplay, eglMainSurface, eglMainSurface, eglMainContext);
  if(!ret)
    CLog::Log(LOGERROR, "MakeCurrent - Could not make surface current(display=%d)",(int)withSurface);
}

CDVDMediaCodecInfo::CDVDMediaCodecInfo( int index , unsigned int texture)
: m_refs(1)
, m_valid(true)
, m_isReleased(true)
, m_index(index)
, m_texture(texture)
, m_timestamp(0)
, m_eglImage(0)
{
  ///CLog::Log(LOGNOTICE, "%s(V#%d, texture %d)", __FUNCTION__, index , texture );
  assert(m_index >= 0);
}

CDVDMediaCodecInfo::~CDVDMediaCodecInfo()
{
  ///CLog::Log(LOGNOTICE, "%s V#%d", __FUNCTION__, m_index );
  assert(m_refs == 0);
}

CDVDMediaCodecInfo* CDVDMediaCodecInfo::Retain()
{
  ++m_refs;
  return this;
}

void CDVDMediaCodecInfo::SetSync()
{
  if (m_eglSync) {
    eglDestroySyncKHR(m_eglDisplay, m_eglSync);
  }
  m_eglSync = eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_FENCE_KHR, NULL);
  glFlush(); // flush to ensure the sync later will succeed
}

void CDVDMediaCodecInfo::WaitSync()
{
  if (m_eglSync) {
    eglClientWaitSyncKHR(m_eglDisplay, m_eglSync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 1*1000*1000*1000);
    eglDestroySyncKHR(m_eglDisplay, m_eglSync);
    m_eglSync = 0;
  }	  
}

long CDVDMediaCodecInfo::Release()
{
  long count = --m_refs;
  ///if (count == 1)
    ///ReleaseOutputBuffer(false);
  if (count == 0) {
    //WaitSync();

    ///ReleaseOutputBuffer(false);
    vpu_buf_t* mem = mems[m_index];
    //if(mem) VPUFreeLinear(mem->vpu_mem);

    delete this;
  }
  return count;
}

ssize_t CDVDMediaCodecInfo::GetIndex() const
{
  CSingleLock lock(m_section);
  return m_index;
}

int CDVDMediaCodecInfo::GetTextureID() const
{
  return m_texture; // threadsafe as texture never changes
}

void CDVDMediaCodecInfo::UpdateTexImage()
{
  CSingleLock lock(m_section);

  vpu_buf_t* mem = mems[m_index];
  if((!m_eglImage) && mem) {
    makeCurrent(true);

    EGLint attrs[] = {
        EGL_WIDTH, 0, EGL_HEIGHT, 0,
        EGL_LINUX_DRM_FOURCC_EXT, 0,
        EGL_DMA_BUF_PLANE0_FD_EXT, 0,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, 0,
        EGL_DMA_BUF_PLANE1_FD_EXT, 0,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE1_PITCH_EXT, 0,
        EGL_YUV_COLOR_SPACE_HINT_EXT, 0,
        EGL_SAMPLE_RANGE_HINT_EXT, 0,
        EGL_NONE,
    };

    #define VA_FOURCC_NV12		0x3231564E
    attrs[1] = mem->width;
    attrs[3] = mem->height;
    attrs[5] = VA_FOURCC_NV12;
    attrs[7] = mem->dma_fd;
    attrs[9] = 0;
    attrs[11] = mem->aligned_width; // pitch
    attrs[13] = mem->dma_fd;
    attrs[15] = mem->aligned_width * mem->aligned_height; // offset
    attrs[17] = mem->aligned_width; // pitch
    attrs[19] = EGL_ITU_REC601_EXT;
    attrs[21] = EGL_YUV_NARROW_RANGE_EXT;

    CHECKEGL();
    m_eglImage = eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT,
                          EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (m_eglImage == EGL_NO_IMAGE_KHR) {
      CLog::Log(LOGERROR, "%s - eglCreateImageKHR() failed", __FUNCTION__);
      return;
    }
    CHECKEGL();

    glActiveTexture(GL_TEXTURE0);
    CHECKEGL();
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    CHECKEGL();
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, m_eglImage);
    CHECKEGL();

    ///CLog::Log(LOGNOTICE, "%s - V#%d, m_eglImage(%x),m_texture(%d)", __FUNCTION__,m_index, (void*)m_eglImage,m_texture);


  //glFlush();
  /*EGLSyncKHR sync;
  sync = eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_FENCE_KHR, NULL);
  FUN();
  if (sync != EGL_NO_SYNC_KHR) {
    FUN();
    EGLint result = eglClientWaitSyncKHR(m_eglDisplay, sync,
		    EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 2000000000 ); // 2 sec
    EGLint eglErr = eglGetError();
    if (result == EGL_TIMEOUT_EXPIRED_KHR) {
      CLog::Log(LOGNOTICE, "%s - fence wait timed out",
		    __FUNCTION__, (int)eglErr);
    } else {
      if(eglErr != EGL_SUCCESS)
        CLog::Log(LOGERROR, "%s - error waiting on EGL fence: %#x",
		    __FUNCTION__, (int)eglErr);
      else { FUN(); }
    }
    eglDestroySyncKHR(m_eglDisplay, sync);
  }*/

/*// ARM-specific extension needed to make Mali GPU behave - not in any
// published header file.
#ifndef EGL_SYNC_PRIOR_COMMANDS_IMPLICIT_EXTERNAL_ARM
#define EGL_SYNC_PRIOR_COMMANDS_IMPLICIT_EXTERNAL_ARM 0x328A
#endif

  glFlush();
  const EGLint attrib[] = {EGL_SYNC_CONDITION_KHR,
                           EGL_SYNC_PRIOR_COMMANDS_IMPLICIT_EXTERNAL_ARM, EGL_NONE};
  EGLSyncKHR sync;
  sync = eglCreateSyncKHR(m_eglDisplay, EGL_SYNC_FENCE_KHR, attrib);
  FUN();
  if (sync != EGL_NO_SYNC_KHR) {
    FUN();
    for (;;) {
      EGLint result = eglClientWaitSyncKHR(m_eglDisplay, sync,
		    EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 0);
      if (result != EGL_TIMEOUT_EXPIRED_KHR) {
        break;
      }
      usleep(99);
    }
    EGLint eglErr = eglGetError();
    if(eglErr != EGL_SUCCESS) {
      CLog::Log(LOGERROR, "%s - error waiting on EGL fence: %#x",
		    __FUNCTION__, (int)eglErr);
    }
    FUN();
    eglDestroySyncKHR(m_eglDisplay, sync);
  }*/

    eglDestroyImageKHR(m_eglDisplay, m_eglImage);
    CHECKEGL();

    VPUFreeLinear(mem->vpu_mem);
  }
}

CDVDVideoCodecRKCodec::CDVDVideoCodecRKCodec(CProcessInfo &processInfo, bool surface_render)
: CDVDVideoCodec(processInfo)
, m_formatname("macrkcodec")
, m_opened(false)
, m_textureId(0)
, m_bitstream(NULL)
, m_render_sw(false)
{
  memset(&m_videobuffer, 0x00, sizeof(DVDVideoPicture));
  memset(&m_demux_pkt, 0x00, sizeof(m_demux_pkt));
}

CDVDVideoCodecRKCodec::~CDVDVideoCodecRKCodec()
{
  Dispose();
}

int RK_Decode( RK_U8 *data, RK_S32 pkt_size, RK_S64 dts, RK_S64 pts ) {
  if(!vpu_ctx) return -1;

  // reuse/(re)alloc packet sent to VPU decoder
  if (pkt->data == NULL) {
    pkt->data = (RK_U8*)(malloc)(pkt_size);
    if (pkt->data ==NULL) {
      CLog::Log(LOGERROR, "%s - malloc() failed", __FUNCTION__);
      return -1;
    }
    pkt->capability = pkt_size;
  }
  if (pkt->capability <((RK_U32)pkt_size)) {
    pkt->data = (RK_U8*)(realloc)((void*)(pkt->data), pkt_size);
    if (pkt->data ==NULL) {
      CLog::Log(LOGERROR, "%s - realloc() failed", __FUNCTION__);
      return -1;
    }
    pkt->capability = pkt_size;
  }

  // copy data and timestamps
  memcpy(pkt->data,data,pkt_size);
  pkt->size = pkt_size;
  pkt->dts = dts;
  pkt->pts = pts;
  pkt->nFlags = VPU_API_DEC_INPUT_SYNC;

  if((ret = vpu_ctx->decode_sendstream(vpu_ctx, pkt)) != 0) {
    CLog::Log(LOGERROR, "%s - vpu_ctx->decode_sendstream() failed",
		    __FUNCTION__);
    return -1;
  }

  return 0;
}

bool CDVDVideoCodecRKCodec::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (s_instances > 0)
    return false;

  // mediacodec crashes with null size. Trap this...
  if (!hints.width || !hints.height)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec::Open - %s\n", "null size, cannot handle");
    return false;
  }
  else if (hints.stills || hints.dvd)
  {
    // Won't work reliably
    return false;
  }
/*  else if (!CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODEC) &&
           !CSettings::GetInstance().GetBool(CSettings::SETTING_VIDEOPLAYER_USEMEDIACODECSURFACE))
    return false;
*/
  m_drop = false;
  m_state = MEDIACODEC_STATE_UNINITIALIZED;
  m_noPictureLoop = 0;
  m_codecControlFlags = 0;
  m_hints = hints;
  m_dec_retcode = VC_BUFFER;

  // validate supported video formats & convert bitstream if needed (h265/hevc)
  VPU_VIDEO_CODINGTYPE videoCoding = VPU_VIDEO_CodingUnused;
  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MPEG2VIDEO:
      m_mime = "video/mpeg2";
      m_formatname = "macrk-mpeg2";
      videoCoding = VPU_VIDEO_CodingMPEG2;
      break;
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MSMPEG4V3:
      m_mime = "video/mp4";
      m_formatname = "macrk-mpeg4";
      videoCoding = VPU_VIDEO_CodingMPEG4;
      break;
    case AV_CODEC_ID_H263:
      m_mime = "video/3gpp";
      m_formatname = "macrk-h263";
      videoCoding = VPU_VIDEO_CodingH263;
      break;
    case AV_CODEC_ID_VP3:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP8:
      //m_mime = "video/x-vp6";
      //m_mime = "video/x-vp7";
      m_mime = "video/x-vp8";
      m_formatname = "macrk-vp8";
      videoCoding = VPU_VIDEO_CodingVP8;
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
    case AV_CODEC_ID_H264:
      switch(hints.profile)
      {
        case FF_PROFILE_H264_HIGH_10:
        case FF_PROFILE_H264_HIGH_10_INTRA:
          // No known h/w decoder supporting Hi10P
	  /// mac-l1: NYI, rk supports hi10p with YUV420P10LE frames
          return false;
      }
      m_mime = "video/avc";
      m_formatname = "macrk-h264";
      videoCoding = VPU_VIDEO_CodingAVC;
      // check for h264-avcC and convert to h264-annex-b
      if (m_hints.extradata)
      {
        m_bitstream = new CBitstreamConverter;
        if (!m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true))
        {
          SAFE_DELETE(m_bitstream);
        }
      }
      else
      {
        CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec::Open - No extradata found");
        return false;
      }
      break;
    case AV_CODEC_ID_HEVC:
      m_mime = "video/hevc";
      m_formatname = "macrk-h265";
      videoCoding = VPU_VIDEO_CodingHEVC;
      // check for hevc-hvcC and convert to h265-annex-b
      if (m_hints.extradata)
      {
        m_bitstream = new CBitstreamConverter;
        if (!m_bitstream->Open(m_hints.codec, (uint8_t*)m_hints.extradata, m_hints.extrasize, true))
        {
          SAFE_DELETE(m_bitstream);
        }
      }
      else
      {
        CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec::Open - No extradata found");
        return false;
      }
      break;
    default:
      CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec::Open - Unknown hints.codec(%d)", hints.codec);
      return false;
      break;
  }

  // open vpu
  int ret = vpu_open_context(&vpu_ctx);
  if (ret || (vpu_ctx==NULL)) {
    CLog::Log(LOGERROR, "%s - vpu_open_context failed, ret = %x", __FUNCTION__, ret);
    return false;
  }

  // init decoder
  vpu_ctx->videoCoding = (OMX_RK_VIDEO_CODINGTYPE)videoCoding;
  vpu_ctx->codecType = CODEC_DECODER;
  vpu_ctx->width = m_hints.width;
  vpu_ctx->height = m_hints.height;
  vpu_ctx->no_thread = 0;
  vpu_ctx->enableparsing = 1;
  if ((ret = vpu_ctx->init(vpu_ctx, NULL, 0))!=0) {
    CLog::Log(LOGERROR, "%s - vpu_ctx->init() failed, ret = %x", __FUNCTION__, ret);
    return false;
  }

  // get format/sizes used by vpu
  vpu_ctx->control (vpu_ctx, VPU_API_DEC_GETFORMAT, &fmt);
  CLog::Log(LOGNOTICE, "%s - VPU info: format %d, aligned_width %d, aligned_height %d, stride %d, size %d", 
    __FUNCTION__, (int)fmt.format, 
    fmt.aligned_width, fmt.aligned_height, fmt.aligned_stride,
    fmt.aligned_frame_size);

  // open mem pool
  int vpu_size = fmt.aligned_frame_size;
  if((vpu_display_pool = open_vpu_memory_pool()) == NULL ) {
    CLog::Log(LOGERROR, "%s - "
      "create_vpu_memory_pool_allocator() failed, ret = %x",
      __FUNCTION__, ret);
    return false;
  }

  // attach mem pool to current vpu context
  vpu_ctx->control(vpu_ctx,VPU_API_SET_VPUMEM_CONTEXT,(void*)
                   vpu_display_pool);

  // dont block when decoding frames, in/out-streams are processed in 1 thread
  VPU_SYNC sync;
  sync.flag = 0;
  // If buffers arent ready, DONT block the dequeue operation
  vpu_ctx->control(vpu_ctx, VPU_API_SET_OUTPUT_BLOCK, (void *)&sync);

  // allocate and index needed vpu frame buffers with respective dma_buf fds
  int i;
  for (i = 0; i < N_VPUBUFS; i++) {
    VPUMemLinear_t *vpu_mem = (VPUMemLinear_t*)malloc(sizeof(VPUMemLinear_t));
    if( !vpu_mem ) {
      CLog::Log(LOGERROR, "%s - malloc() failed", __FUNCTION__);
      return false;
    }

    VPUMemLinear_t temp_vpu_mem;
    // Create vpumem from mpp/libvpu included mvc data 
    VPUMallocLinearOutside (&temp_vpu_mem, vpu_size);
    memcpy( vpu_mem, &temp_vpu_mem, sizeof (VPUMemLinear_t) );
    vpu_mem->index = vpu_display_pool_count;

    ret = vpu_display_pool->commit_vpu (vpu_display_pool, vpu_mem);
    // Release the old buffer reference in the other memory group 
    VPUFreeLinear (&temp_vpu_mem);
    if (ret > 0) vpu_display_pool->inc_used (vpu_display_pool, vpu_mem);

    vpu_buf_t* vpu_buf = mems[i] = (vpu_buf_t*)malloc(sizeof(vpu_buf_t ) );
    if( vpu_buf == NULL ) {
      CLog::Log(LOGERROR, "%s - malloc() failed", __FUNCTION__);
      return false;
    }

    vpu_buf->vpu_mem = vpu_mem;
    vpu_buf->size = vpu_mem->size;
    vpu_buf->dma_fd = VPUMemGetFD(vpu_mem);
    vpu_buf->videoformat = vpu_ctx->videoCoding ;
    vpu_buf->frameformat = fmt.format;
    vpu_buf->width = fmt.width;
    vpu_buf->height = fmt.height;
    vpu_buf->aligned_width = fmt.aligned_width;
    vpu_buf->aligned_height = fmt.aligned_height;

    free_queue.push( mems[i] );
    vpu_display_pool_count++;
  }

  // free the bufs again to make them accessible for vpu decoding
  for (i = 0; i < N_VPUBUFS; i++)  VPUFreeLinear(mems[i]->vpu_mem);

  // init communication packet to access vpu
  memset(&access_unit, 0, sizeof(VideoPacket_t));
  pkt = &access_unit;
  pkt->data = NULL;
  pkt->pts = VPU_API_NOPTS_VALUE;
  pkt->dts = VPU_API_NOPTS_VALUE;

  m_codec = vpu_ctx;
  m_colorFormat = fmt.format; // Save color format for initial output configuration

  // setup a YUV420(S)P DVDVideoPicture buffer.
  // first make sure all properties are reset.
  memset(&m_videobuffer, 0x00, sizeof(DVDVideoPicture));
  m_videobuffer.dts = DVD_NOPTS_VALUE;
  m_videobuffer.pts = DVD_NOPTS_VALUE;
  m_videobuffer.color_range  = 0;
  m_videobuffer.color_matrix = 4;
  m_videobuffer.iFlags  = DVP_FLAG_ALLOCATED;
  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;
  // these will get reset to crop values later
  m_videobuffer.iDisplayWidth  = m_hints.width;
  m_videobuffer.iDisplayHeight = m_hints.height;

  // handle codec extradata
  if (m_hints.extrasize)
  {
    size_t size = m_hints.extrasize;
    void  *src_ptr = m_hints.extradata;
    if (m_bitstream)
    {
      size = m_bitstream->GetExtraSize();
      src_ptr = m_bitstream->GetExtraData();
    }

    // send codec extradata to VPU decoder
    if( (ret = RK_Decode((RK_U8*)src_ptr, size, 0, 0)) < 0 ) {
      CLog::Log(LOGERROR, "%s - failed to send codec extradata, ret = %x",
        __FUNCTION__, ret);
      return false;
    }
  }

  m_render_sw = false;
  ///m_render_sw = true;

  InitSurfaceTexture();

  m_state = MEDIACODEC_STATE_CONFIGURED;
  m_state = MEDIACODEC_STATE_FLUSHED;

  // Configure the output with defaults
  ConfigureOutputFormat(&fmt);

  s_instances++;
  m_opened = true;
  memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));

  m_processInfo.SetVideoDecoderName(m_formatname, true );
  m_processInfo.SetVideoPixelFormat(m_render_sw ? "YUV" : "EGLIMAGE");
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDAR(m_hints.aspect);
  m_processInfo.SetVideoDeintMethod("none");
  std::list<EINTERLACEMETHOD> deintMethods;
  deintMethods.push_back(EINTERLACEMETHOD::VS_INTERLACEMETHOD_RENDER_BOB);
  m_processInfo.UpdateDeinterlacingMethods(deintMethods);

  CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: "
    "Open codec %s (%d,'%s')",m_codecname.c_str(), 
    m_render_sw, m_render_sw ? "YUV" : "EGLIMAGE");

  return m_opened;
}

void CDVDVideoCodecRKCodec::Dispose()
{
  if (vpu_ctx) {
    // send EOS to hw decoder
    VideoPacket_t pkt;
    RK_S32 is_eos = 0;
    pkt.size = 0;
    pkt.data = NULL;
    pkt.nFlags = VPU_API_DEC_OUTPUT_EOS;
    vpu_ctx->decode_sendstream (vpu_ctx, &pkt);
    vpu_ctx->control (vpu_ctx, VPU_API_DEC_GET_EOS_STATUS, &is_eos);

    // close hw decoder session
    vpu_close_context(&vpu_ctx);
    vpu_ctx = NULL;

    // free queue mem
    while(!free_queue.empty()) free_queue.pop();
    for (int i = 0; i <vpu_display_pool_count ; i++) {
        vpu_buf_t* mem = mems[i];
        mems[i] = NULL;
        if (mem) free (mem);
    }
    vpu_display_pool_count = 0;

    // free/close vpu mempool
    if( vpu_display_pool ) close_vpu_memory_pool(vpu_display_pool);
    vpu_display_pool = NULL;
  }

  SAFE_DELETE(m_bitstream);
  s_instances = 0;

  if (!m_opened)
    return;
  m_opened = false;

  // release any retained demux packets
  if (m_demux_pkt.pData)
    free(m_demux_pkt.pData);

  // invalidate any inflight outputbuffers
  FlushInternal();

  // clear m_videobuffer bits
  if (m_render_sw)
  {
    free(m_videobuffer.data[0]), m_videobuffer.data[0] = NULL;
    free(m_videobuffer.data[1]), m_videobuffer.data[1] = NULL;
    free(m_videobuffer.data[2]), m_videobuffer.data[2] = NULL;
  }
  m_videobuffer.iFlags = 0;
  // m_videobuffer.mediacodec is unioned with m_videobuffer.data[0]
  // so be very careful when and how you touch it.
  m_videobuffer.mediacodec = NULL;

  if (m_codec)
  {
    ///m_codec->stop();
    m_state = MEDIACODEC_STATE_UNINITIALIZED;
    m_codec = NULL;
    /*
    m_codec->release();
    m_codec.reset();*/
  }

  ReleaseSurfaceTexture();
}

int CDVDVideoCodecRKCodec::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
  if (!m_opened)
    return VC_ERROR;

  if (m_state != MEDIACODEC_STATE_RUNNING)
    CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec::Decode current state (%d)", m_state);

  if (m_hints.ptsinvalid)
    pts = DVD_NOPTS_VALUE;

  // Handle input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as VideoPlayerVideo has no concept of "try again".
  // we must return VC_BUFFER or VC_PICTURE, default to VC_BUFFER.
  bool drain = (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN) ? true : false;
  m_dec_retcode = (drain) ? 0 : VC_BUFFER;

  if (!pData)
  {
    // If no data then check if we have a saved buffer
    if (m_demux_pkt.pData && m_state != MEDIACODEC_STATE_ENDOFSTREAM)
    {
      pData = m_demux_pkt.pData;
      iSize = m_demux_pkt.iSize;
      pts = m_demux_pkt.pts;
      dts = m_demux_pkt.dts;
    }
  }
  else if (m_state == MEDIACODEC_STATE_ENDOFSTREAM)
  {
    // We received a packet but already reached EOS. Flush...
    FlushInternal();
    if(vpu_ctx) vpu_ctx->flush(vpu_ctx);
    m_state = MEDIACODEC_STATE_FLUSHED;
    m_dec_retcode |= VC_BUFFER;
  }

  // must check for an output picture 1st,
  // otherwise, mediacodec can stall on some devices.
  int retgp = GetOutputPicture();
  if (retgp > 0)
  {
    m_dec_retcode |= VC_PICTURE;
    m_noPictureLoop = 0;
  }
  else if (retgp == -1 || (drain && ++m_noPictureLoop == 10))  // EOS
  {
    m_state = MEDIACODEC_STATE_ENDOFSTREAM;
    m_dec_retcode |= VC_BUFFER;
    m_noPictureLoop = 0;
  }

  if (pData)
  {
    if (m_state == MEDIACODEC_STATE_FLUSHED)
      m_state = MEDIACODEC_STATE_RUNNING;
    if (!(m_state == MEDIACODEC_STATE_FLUSHED || m_state == MEDIACODEC_STATE_RUNNING))
      CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec::Decode - wrong state (%d)", m_state);

    // if needed convert to bitstream
    if (m_bitstream)
    {
      m_bitstream->Convert(pData, iSize);
      iSize = m_bitstream->GetConvertSize();
      pData = m_bitstream->GetConvertBuffer();
    }

    // Translate from VideoPlayer dts/pts to MediaCodec pts,
    // pts WILL get re-ordered by MediaCodec if needed.
    // Do not try to pass pts as a unioned double/int64_t,
    // some android devices will diddle with presentationTimeUs
    // and you will get NaN back and VideoPlayerVideo will barf.
    int64_t presentationTimeUs = 0;
    if (pts != DVD_NOPTS_VALUE)
      presentationTimeUs = pts;
    else if (dts != DVD_NOPTS_VALUE)
      presentationTimeUs = dts;
/*
    CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: "
      "pts(%f), ipts(%lld), iSize(%d), GetDataSize(%d), loop_cnt(%d)",
      presentationTimeUs, pts_dtoi(presentationTimeUs), iSize, GetDataSize(), loop_cnt);
*/
    if( RK_Decode(pData, iSize, dts, presentationTimeUs) >= 0 ) {
      // Free saved buffer if there was one
      if (m_demux_pkt.pData)
      {
        free(m_demux_pkt.pData);
        memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));
      }
    }
    else
    {
      // We couldn't put input data somehow. Save the packet for next iteration, if it wasn't already
      if (!m_demux_pkt.pData)
      {
        m_demux_pkt.dts = dts;
        m_demux_pkt.pts = pts;
        m_demux_pkt.iSize = iSize;
        m_demux_pkt.pData = (uint8_t*)malloc(iSize);
        memcpy(m_demux_pkt.pData, pData, iSize);
      }

      m_dec_retcode &= ~VC_BUFFER;
    }
  }

  return m_dec_retcode;
}

void CDVDVideoCodecRKCodec::Reset()
{
  FUN();

  if (!m_opened)
    return;

  // dump any pending demux packets
  if (m_demux_pkt.pData)
  {
    free(m_demux_pkt.pData);
    memset(&m_demux_pkt, 0, sizeof(m_demux_pkt));
  }

  if (m_codec)
  {
    // flush all outputbuffers inflight, they will
    // become invalid on m_codec->flush and generate
    // a spew of java exceptions if used
    FlushInternal();

    // now we can flush the actual MediaCodec object
    CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec::Reset Current state (%d)", m_state);
    if(vpu_ctx) vpu_ctx->flush(vpu_ctx);
    m_state = MEDIACODEC_STATE_FLUSHED;

    // Invalidate our local DVDVideoPicture bits
    m_videobuffer.pts = DVD_NOPTS_VALUE;
    if (!m_render_sw)
      m_videobuffer.mediacodec = NULL;
  }
}

bool CDVDVideoCodecRKCodec::GetPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (!m_opened)
    return false;

  *pDvdVideoPicture = m_videobuffer;

  // Invalidate our local DVDVideoPicture bits
  m_videobuffer.pts = DVD_NOPTS_VALUE;
  if (!m_render_sw)
    m_videobuffer.mediacodec = NULL;

  return true;
}

bool CDVDVideoCodecRKCodec::ClearPicture(DVDVideoPicture* pDvdVideoPicture)
{
  if (pDvdVideoPicture->format == RENDER_FMT_ROCKCHIP)
    SAFE_RELEASE(pDvdVideoPicture->mediacodec);
  memset(pDvdVideoPicture, 0x00, sizeof(DVDVideoPicture));

  return true;
}

void CDVDVideoCodecRKCodec::SetDropState(bool bDrop)
{
  if (bDrop == m_drop)
    return;

  CLog::Log(LOGNOTICE, "%s::%s %s->%s", "CDVDVideoCodecRKCodec", __func__, m_drop ? "true" : "false", bDrop ? "true" : "false");

  m_drop = bDrop;
  if (m_drop)
    m_videobuffer.iFlags |=  DVP_FLAG_DROPPED;
  else
    m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;
}

void CDVDVideoCodecRKCodec::SetCodecControl(int flags)
{
  if (m_codecControlFlags != flags)
  {
    CLog::Log(LOGNOTICE, "%s::%s %x->%x", "CDVDVideoCodecRKCodec", __func__, m_codecControlFlags, flags);
    m_codecControlFlags = flags;
  }
}

int CDVDVideoCodecRKCodec::GetDataSize(void)
{
  // just ignore internal buffering contribution.
  return 0;
}

double CDVDVideoCodecRKCodec::GetTimeSize(void)
{
  // just ignore internal buffering contribution.
  return 0.0;
}

unsigned CDVDVideoCodecRKCodec::GetAllowedReferences()
{
  return 8;
}

void CDVDVideoCodecRKCodec::FlushInternal()
{
  // invalidate any existing inflight buffers and create
  // new ones to match the number of output buffers

  if (m_render_sw)
    return;
/*
  for (size_t i = 0; i < m_inflight.size(); i++)
  {
    m_inflight[i]->Validate(false);
    m_inflight[i]->Release();
  }
  m_inflight.clear();
  */
}

int CDVDVideoCodecRKCodec::GetOutputPicture(void)
{
  int rtn = 0, ret = 0, index = -1;

  DecoderOut_t dec_out;
  VPU_FRAME vpu_frame;
  VPUMemLinear_t vpu_mem;
  memset (&dec_out, 0, sizeof (DecoderOut_t));
  memset (&vpu_frame, 0, sizeof (VPU_FRAME));
  dec_out.data = (RK_U8*)&vpu_frame;

  if ((ret = vpu_ctx->decode_getframe (vpu_ctx, &dec_out)) < 0) {
    if( ret == VPU_API_EOS_STREAM_REACHED ) {
      CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: VPU_API_EOS_STREAM_REACHED");
      // mac-l1: seems from vpu_api_legacy.cpp that eos with vpu frame buf 
      // is possible so vpu frame has to be released???
      if(vpu_frame.vpumem.vir_addr) /// not memset zeroed anymore means used
        VPUFreeLinear(&vpu_frame.vpumem); /// so free

      return -1;
    }
    CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec:: vpu_ctx->decode_getframe() failed, ret = %d", ret);
    return -2;
  } else {
    if(dec_out.size != 0) {
      memcpy( &vpu_mem, &vpu_frame.vpumem,sizeof(vpu_mem));
      index = vpu_mem.index;
      vpu_buf_t* mem = mems[index];

      /* if(!(avctx->frame_number % 100)) {
        int* pARGB = (int*)malloc(
          sizeof(int)*mem->width*mem->height );
          decodeYUV420SP(pARGB, (RK_U8*)(vpu_frame.vpumem.vir_addr),
          mem->width, mem->height,
          mem->aligned_width, mem->aligned_height);
        write_truecolor_tga( pARGB, mem->width, mem->height,
          avctx->frame_number ) ;
        free(pARGB);
      } */

      if (vpu_frame.info_change) {
        // get format/sizes from vpu
        vpu_ctx->control (vpu_ctx, VPU_API_DEC_GETFORMAT, &fmt);
        CLog::Log(LOGNOTICE, "%s - VPU info change: format %d, aligned_width %d, aligned_height %d, stride %d, size %d", 
          __FUNCTION__, (int)fmt.format, 
          fmt.aligned_width, fmt.aligned_height, fmt.aligned_stride,
          fmt.aligned_frame_size);

        ConfigureOutputFormat(&fmt);
      }

      int64_t pts= dec_out.timeUs;
      m_videobuffer.dts = DVD_NOPTS_VALUE;
      m_videobuffer.pts = DVD_NOPTS_VALUE;
      if ( (pts != 0) && (pts != -1) ) // values from scan mpp source code
        m_videobuffer.pts = pts;

      if (0||m_drop) // dont display, just drop it
      {
        VPUFreeLinear(mem->vpu_mem);
	///mac-l1: .mediacodec is unioned with .data[] so dont zero it here!
        ///m_videobuffer.mediacodec = nullptr;
        return 0;
      }

      if (!m_render_sw)
      {
        /*size_t i = 0;
        for (; i < m_inflight.size(); ++i)
        {
          if (m_inflight[i]->GetIndex() == index)
            break;
        }
        if (i == m_inflight.size())
          m_inflight.push_back(
            new CDVDMediaCodecInfo(index, m_textureId , m_codec, m_surfaceTexture, m_frameAvailable)
          );
        m_videobuffer.mediacodec = m_inflight[i]->Retain();
	*/
        m_videobuffer.mediacodec = new CDVDMediaCodecInfo(index, m_textureId );
        //m_videobuffer.mediacodec->Retain();
      }
      else
      {
        uint8_t *src_ptr = (uint8_t*)vpu_frame.vpumem.vir_addr;

        int loop_end = 0;
        if (m_videobuffer.format == RENDER_FMT_NV12)
          loop_end = 2;
        else if (m_videobuffer.format == RENDER_FMT_YUV420P)
          loop_end = 3;
  
        for (int i = 0; i < loop_end; i++)
        {
          uint8_t *src   = src_ptr + m_src_offset[i];
          int src_stride = m_src_stride[i];
          uint8_t *dst   = m_videobuffer.data[i];
          int dst_stride = m_videobuffer.iLineSize[i];
  
          int height = m_videobuffer.iHeight;
          if (i > 0)
            height = (m_videobuffer.iHeight + 1) / 2;

          if (src_stride == dst_stride)
            memcpy(dst, src, dst_stride * height);
          else
            for (int j = 0; j < height; j++, src += src_stride, dst += dst_stride)
              memcpy(dst, src, dst_stride);
        }

        VPUFreeLinear(mem->vpu_mem);
      }

/*
    CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec::GetOutputPicture "
      "index(%d), pts(%f)", index, m_videobuffer.pts);
*/
      rtn = 1;
    } 
    else
    {
      // we should never get here
      CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec::GetOutputPicture unknown error");
      rtn = -2;
    }
  }

  return rtn;
}

void CDVDVideoCodecRKCodec::ConfigureOutputFormat(DecoderFormat_t* mediaformat)
{
  int width       = mediaformat->width;
  int height      = mediaformat->height;
  int stride      = mediaformat->aligned_width;
  int slice_height= mediaformat->aligned_height;
  int color_format= mediaformat->format;
  int crop_left   = 0;
  int crop_top    = 0;
  int crop_right  = 0;
  int crop_bottom = 0;

  CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: "
    "width(%d), height(%d), stride(%d), slice-height(%d), color-format(%d)",
    width, height, stride, slice_height, color_format);
  CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: "
    "crop-left(%d), crop-top(%d), crop-right(%d), crop-bottom(%d)",
    crop_left, crop_top, crop_right, crop_bottom);

  if (stride <= width)
    stride = width;
  if (!crop_right)
    crop_right = width-1;
  if (!crop_bottom)
    crop_bottom = height-1;

  if (!m_render_sw)
  {
      CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: zerocopy rendering");
      m_videobuffer.format = RENDER_FMT_ROCKCHIP;
  }
  else
  {
    // default picture format to none
    for (int i = 0; i < 4; i++)
      m_src_offset[i] = m_src_stride[i] = 0;
    // delete any existing buffers
    for (int i = 0; i < 4; i++)
      free(m_videobuffer.data[i]);

    // setup picture format and data offset vectors
    if (1 || color_format == fmt.format ) // XXX always true, just to log format and hardcoded value later...
    {
      CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: rockchip color_format(%d) supported", color_format);

      // Y plane
      m_src_stride[0] = stride;
      m_src_offset[0] = crop_top * stride;
      m_src_offset[0]+= crop_left;

      // UV plane
      m_src_stride[1] = stride;
      //  skip over the Y plane
      m_src_offset[1] = slice_height * stride;
      m_src_offset[1]+= crop_top * stride;
      m_src_offset[1]+= crop_left;

      m_videobuffer.iLineSize[0] = width;  // Y
      m_videobuffer.iLineSize[1] = width;  // UV
      m_videobuffer.iLineSize[2] = 0;
      m_videobuffer.iLineSize[3] = 0;

      unsigned int iPixels = width * height;
      unsigned int iChromaPixels = iPixels;
      m_videobuffer.data[0] = (uint8_t*)malloc(16 + iPixels);
      m_videobuffer.data[1] = (uint8_t*)malloc(16 + iChromaPixels);
      m_videobuffer.data[2] = NULL;
      m_videobuffer.data[3] = NULL;
      m_videobuffer.format  = RENDER_FMT_NV12;
    }
    else
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec:: unknown color_format(%d)", color_format);
      return;
    }
  }

  if (width)
    m_videobuffer.iWidth  = width;
  if (height)
    m_videobuffer.iHeight = height;

  // picture display width/height include the cropping.
  m_videobuffer.iDisplayWidth  = crop_right  + 1 - crop_left;
  m_videobuffer.iDisplayHeight = crop_bottom + 1 - crop_top;
  if (m_hints.aspect > 1.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }
}

void CDVDVideoCodecRKCodec::CallbackInitSurfaceTexture(void *userdata)
{
  CDVDVideoCodecRKCodec *ctx = static_cast<CDVDVideoCodecRKCodec*>(userdata);
  ctx->InitSurfaceTexture();
}

void CDVDVideoCodecRKCodec::InitSurfaceTexture(void)
{
  if (m_render_sw )
    return;

  // We MUST create the GLES texture on the main thread
  // to match where the valid GLES context is located.
  // It would be nice to move this out of here, we would need
  // to create/fetch/create from g_RenderMananger. But g_RenderMananger
  // does not know we are using MediaCodec until Configure and we
  // we need m_surfaceTexture valid before then. Chicken, meet Egg.
  if (g_application.IsCurrentThread())
  {
    // localize GLuint so we do not spew gles includes in our header
    GLuint texture_id;

    glGenTextures(1, &texture_id);
    glBindTexture(  GL_TEXTURE_EXTERNAL_OES, texture_id);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(  GL_TEXTURE_EXTERNAL_OES, 0);
    m_textureId = texture_id;

    CLog::Log(LOGNOTICE, "CDVDVideoCodecRKCodec:: textureid(%d)", texture_id);
    ///m_surfaceTexture = std::shared_ptr<CJNISurfaceTexture>(new CJNISurfaceTexture(m_textureId));
    // hook the surfaceTexture OnFrameAvailable callback
    ///m_frameAvailable = std::shared_ptr<CDVDMediaCodecOnFrameAvailable>(new CDVDMediaCodecOnFrameAvailable(m_surfaceTexture));
    ///m_surface = new CJNISurface(*m_surfaceTexture);
  /*if (!CreateEGLContext()) {
    CLog::Log(LOGERROR, "CDVDVideoCodecRKCodec::Open - %s\n", "cannot create egl context");
    ///return false;
  }*/

  }
  else
  {
    ThreadMessageCallback callbackData;
    callbackData.callback = &CallbackInitSurfaceTexture;
    callbackData.userptr  = (void*)this;

    // wait for it.
    CApplicationMessenger::GetInstance().SendMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&callbackData));
  }

  return;
}

void CDVDVideoCodecRKCodec::ReleaseSurfaceTexture(void)
{
  FIN();

  if (m_render_sw )
    return;

  if (m_textureId > 0)
  {
    GLuint texture_id = m_textureId;
    glDeleteTextures(1, &texture_id);
    m_textureId = 0;
  }
}
