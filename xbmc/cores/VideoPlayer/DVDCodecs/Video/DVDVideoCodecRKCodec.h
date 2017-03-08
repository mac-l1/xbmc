#pragma once
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

#include "system.h"

#include <queue>
#include <vector>
#include <memory>

#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "threads/Thread.h"
#include "threads/SingleLock.h"
///#include "platform/android/jni/Surface.h"
#include "guilib/Geometry.h"

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES

///#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "system_gl.h"
#include "windowing/WindowingFactory.h"

#include <cassert>

#define CHECKEGL() { int e=glGetError(); if (e) \
        CLog::Log(LOGERROR, "%s#%d - EGL ERROR %d(0x%x)", \
        __FUNCTION__, __LINE__, e, e);}

extern "C" {
#include "rockchip/vpu_api.h"
}

///class CJNISurface;
///class CJNISurfaceTexture;
///class CJNIMediaCodec;
///class CJNIMediaFormat;
///class CDVDMediaCodecOnFrameAvailable;
///class CJNIByteBuffer;
class CBitstreamConverter;

#define FIN() CLog::Log(LOGNOTICE, "-> %s#%d", __PRETTY_FUNCTION__, __LINE__);
#define FOUT() CLog::Log(LOGNOTICE, "<- %s#%d", __PRETTY_FUNCTION__, __LINE__);
#define FUN() CLog::Log(LOGNOTICE, "== %s#%d", __PRETTY_FUNCTION__, __LINE__);

typedef struct amc_demux {
  uint8_t  *pData;
  int       iSize;
  double    dts;
  double    pts;
} amc_demux;


class CDVDMediaCodecInfo
{
public:
  CDVDMediaCodecInfo( int index,
                      unsigned int texture //,
                      ///std::shared_ptr<CJNIMediaCodec> &codec,
                      ///std::shared_ptr<CJNISurfaceTexture> &surfacetexture,
                      ///std::shared_ptr<CDVDMediaCodecOnFrameAvailable> &frameready
		      );

  // reference counting
  CDVDMediaCodecInfo* Retain();
  long                Release();
  void                SetSync();
  void                WaitSync();

  // meat and potatos
  void                Validate(bool state);
  // MediaCodec related
  void                ReleaseOutputBuffer(bool render);
  bool                IsReleased() { return m_isReleased; }
  // SurfaceTexture released
  ssize_t             GetIndex() const;
  int                 GetTextureID() const;
  void                UpdateTexImage();
  void                RenderUpdate(const CRect &SrcRect, const CRect &DestRect);

  // surface handling functions
  static void     CallbackReleaseOutputBuffer(void*);
  void            ReleaseOutputBuffer(void);

private:
  // private because we are reference counted
  virtual            ~CDVDMediaCodecInfo();

  long                m_refs;
  bool                m_valid;
  bool                m_isReleased;
  int                 m_index;
  unsigned int        m_texture;
  int64_t             m_timestamp;
  CCriticalSection    m_section;

  EGLImageKHR         m_eglImage;
  EGLSyncKHR          m_eglSync;

};

class CDVDVideoCodecRKCodec : public CDVDVideoCodec
{
public:
  CDVDVideoCodecRKCodec(CProcessInfo &processInfo, 
		  bool surface_render = false);
  virtual ~CDVDVideoCodecRKCodec();

  // track instances - we can only allow exactly one
  static int s_instances;
  // required overrides
  virtual bool    Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual int     Decode(uint8_t *pData, int iSize, double dts, double pts);
  virtual void    Reset();
  virtual bool    GetPicture(DVDVideoPicture *pDvdVideoPicture);
  virtual bool    ClearPicture(DVDVideoPicture* pDvdVideoPicture);
  virtual void    SetDropState(bool bDrop);
  virtual void    SetCodecControl(int flags);
  virtual int     GetDataSize(void);
  virtual double  GetTimeSize(void);
  virtual const char* GetName(void) { return m_formatname.c_str(); }
  virtual unsigned GetAllowedReferences();

protected:
  bool CreateEGLContext();
  bool DestroyEGLContext();

  void            Dispose();
  void            FlushInternal(void);
  bool            ConfigureMediaCodec(void);
  int             GetOutputPicture(void);
  void            ConfigureOutputFormat(DecoderFormat_t* mediaformat);

  // surface handling functions
  static void     CallbackInitSurfaceTexture(void*);
  void            InitSurfaceTexture(void);
  void            ReleaseSurfaceTexture(void);

  CDVDStreamInfo  m_hints;
  std::string     m_mime;
  std::string     m_codecname;
  int             m_colorFormat;
  std::string     m_formatname;
  bool            m_opened;
  bool            m_drop;
  int             m_codecControlFlags;
  int             m_state;
  int             m_noPictureLoop;

  ///CJNISurface    *m_surface;
  unsigned int    m_textureId;
  ///CJNISurface     m_videosurface;
  // we need these as shared_ptr because CDVDVideoCodecRKCodec
  // will get deleted before CLinuxRendererGLES is shut down and
  // CLinuxRendererGLES refs them via CDVDMediaCodecInfo.
  ///std::shared_ptr<CJNIMediaCodec> m_codec;
  struct VpuCodecContext* m_codec;
  ///std::shared_ptr<CJNISurfaceTexture> m_surfaceTexture;
  ///std::shared_ptr<CDVDMediaCodecOnFrameAvailable> m_frameAvailable;

  amc_demux m_demux_pkt;
  ///std::vector<CJNIByteBuffer> m_input;
  ///std::vector<CJNIByteBuffer> m_output;
  std::vector<CDVDMediaCodecInfo*> m_inflight;

  CBitstreamConverter *m_bitstream;
  DVDVideoPicture m_videobuffer;

  int             m_dec_retcode;
  bool            m_render_sw;
  int             m_src_offset[4];
  int             m_src_stride[4];

  ///EGLDisplay m_eglDisplay;
  ///EGLSurface m_eglSurface;
  ///EGLContext m_eglContext;
  ///Display *m_Display;
};
