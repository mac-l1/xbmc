/*
 *      Copyright (C) 2007-2015 Team Kodi
 *      http://kodi.tv
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
 *  along with Kodi; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "RendererRKCodec.h"

#if defined(HAVE_ROCKCHIP)
#include "cores/IPlayer.h"
#include "DVDCodecs/Video/DVDVideoCodecRKCodec.h"
#include "utils/log.h"
#include "utils/GLUtils.h"
#include "settings/MediaSettings.h"
#include "windowing/WindowingFactory.h"

#undef DEBUG_VERBOSE
//#define DEBUG_VERBOSE

CRendererRKCodec::CRendererRKCodec()
{
}

CRendererRKCodec::~CRendererRKCodec()
{
}

void CRendererRKCodec::AddVideoPictureHW(DVDVideoPicture &picture, int index)
{
#ifdef DEBUG_VERBOSE
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif
  int mindex = -1;

  YUVBUFFER &buf = m_buffers[index];
  if(buf.hwDec) 
    ((CDVDMediaCodecInfo *)buf.hwDec)->Release();
  buf.hwDec = NULL;

  if (picture.mediacodec) {
    buf.hwDec = picture.mediacodec->Retain();
    mindex = ((CDVDMediaCodecInfo *)buf.hwDec)->GetIndex();

    ((CDVDMediaCodecInfo *)buf.hwDec)->UpdateTexImage();
  }

#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, V#%d, mediacodec %p, tm:%d",__FUNCTION__,__LINE__, index, mindex,m_buffers[index].hwDec , XbmcThreads::SystemClockMillis() - time);
#endif
}

bool CRendererRKCodec::RenderUpdateCheckForEmptyField()
{
  return false;
}

void CRendererRKCodec::ReleaseBuffer(int index)
{
  YUVBUFFER &buf = m_buffers[index];
  int mindex = buf.hwDec ? ((CDVDMediaCodecInfo *)buf.hwDec)->GetIndex() : -1;
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, V#%d, mediacodec %p", __FUNCTION__,__LINE__ ,index, mindex, m_buffers[index].hwDec );
#endif

  if (buf.hwDec)
    ((CDVDMediaCodecInfo *)buf.hwDec)->Release();
  buf.hwDec = NULL;
}

bool CRendererRKCodec::Supports(EINTERLACEMETHOD method)
{
  return method == VS_INTERLACEMETHOD_RENDER_BOB;
}

EINTERLACEMETHOD CRendererRKCodec::AutoInterlaceMethod()
{
  return VS_INTERLACEMETHOD_RENDER_BOB;
}

CRenderInfo CRendererRKCodec::GetRenderInfo()
{
  CRenderInfo info;
  info.formats = m_formats;
  info.max_buffer_size = 4;
  info.optimal_buffer_size = 4;
  return info;
}

bool CRendererRKCodec::LoadShadersHook()
{
  CLog::Log(LOGNOTICE, "GL: Using rockchip render method");
  m_textureTarget = GL_TEXTURE_2D;
  m_renderMethod = RENDER_ROCKCHIP;
  return false;
}

bool CRendererRKCodec::UploadTexture(int index)
{
#ifdef DEBUG_VERBOSE
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif

  YUVBUFFER &buf = m_buffers[index];
  int mindex = buf.hwDec ? ((CDVDMediaCodecInfo *)buf.hwDec)->GetIndex() : -1;

  if (buf.hwDec)
  {
    CDVDMediaCodecInfo *mci = static_cast<CDVDMediaCodecInfo *>(buf.hwDec);
    buf.fields[0][0].id = mci->GetTextureID();
    /*
    mci->SetSync();
    mci->UpdateTexImage();
    mci->WaitSync();
    */
  } 

  CalculateTextureSourceRects(index, 1);

#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "UploadTexture R#%d, V#%d, T#%d, mediacodec %p, tm:%d", index, mindex, buf.fields[0][0].id, buf.hwDec, XbmcThreads::SystemClockMillis() - time);
#endif

  return true;
}

bool CRendererRKCodec::RenderHook(int index)
{
#ifdef DEBUG_VERBOSE
  unsigned int time = XbmcThreads::SystemClockMillis();
#endif

  YUVBUFFER &buf = m_buffers[index];
  int mindex = buf.hwDec ? ((CDVDMediaCodecInfo *)buf.hwDec)->GetIndex() : -1;
  YUVPLANE &plane = m_buffers[index].fields[0][0];
  YUVPLANE &planef = m_buffers[index].fields[index][0];

#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, V#%d, T#%d, mediacodec %p", __FUNCTION__,__LINE__, index, mindex, plane.id, m_buffers[index].hwDec );
#endif

  glDisable(GL_DEPTH_TEST);

  glEnable(GL_TEXTURE_EXTERNAL_OES);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, plane.id);

  g_Windowing.EnableGUIShader(SM_TEXTURE_RGBA_OES);

  GLint   contrastLoc = g_Windowing.GUIShaderGetContrast();
  glUniform1f(contrastLoc, CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Contrast * 0.02f);
  GLint   brightnessLoc = g_Windowing.GUIShaderGetBrightness();
  glUniform1f(brightnessLoc, CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Brightness * 0.01f - 0.5f);

  const float identity[16] = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f
  };
  glUniformMatrix4fv(g_Windowing.GUIShaderGetCoord0Matrix(),  1, GL_FALSE, identity);

  GLubyte idx[4] = {0, 1, 3, 2};        //determines order of triangle strip
  GLfloat ver[4][4];
  GLfloat tex[4][4];

  GLint   posLoc = g_Windowing.GUIShaderGetPos();
  GLint   texLoc = g_Windowing.GUIShaderGetCoord0();

  glVertexAttribPointer(posLoc, 4, GL_FLOAT, 0, 0, ver);
  glVertexAttribPointer(texLoc, 4, GL_FLOAT, 0, 0, tex);

  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  // Set vertex coordinates
  for(int i = 0; i < 4; i++)
  {
    ver[i][0] = m_rotatedDestCoords[i].x;
    ver[i][1] = m_rotatedDestCoords[i].y;
    ver[i][2] = 0.0f;        // set z to 0
    ver[i][3] = 1.0f;
  }

  // Set texture coordinates
  tex[0][0] = tex[3][0] = plane.rect.x1;
  tex[0][1] = tex[1][1] = plane.rect.y1;
  tex[1][0] = tex[2][0] = plane.rect.x2;
  tex[2][1] = tex[3][1] = plane.rect.y2;

  for(int i = 0; i < 4; i++)
  {
    tex[i][2] = 0.0f;
    tex[i][3] = 1.0f;
  }

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

  g_Windowing.DisableGUIShader();
  VerifyGLState();

  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  VerifyGLState();

#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "RenderMediaCodecImage R#%d: tm:%d", index, XbmcThreads::SystemClockMillis() - time);
#endif

  //if( buf.hwDec )
    //((CDVDMediaCodecInfo *)buf.hwDec)->SetSync();

  return true;
}

void CRendererRKCodec::AfterRenderHook(int index)
{
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, mediacodec %p", __FUNCTION__,__LINE__, index, m_buffers[index].hwDec );
#endif
}

bool CRendererRKCodec::RenderUpdateVideoHook(bool clear, DWORD flags, DWORD alpha) 
{
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d", __FUNCTION__,__LINE__ );
#endif
  return false; 
}

int CRendererRKCodec::GetImageHook(YV12Image *image, int source, bool readonly)
{
  return source;
}

bool CRendererRKCodec::CreateTexture(int index)
{
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, mediacodec %p", __FUNCTION__, __LINE__, index, m_buffers[index].hwDec );
#endif

  YV12Image &im     = m_buffers[index].image;
  YUVFIELDS &fields = m_buffers[index].fields;

  memset(&im    , 0, sizeof(im));
  memset(&fields, 0, sizeof(fields));

  im.height = m_sourceHeight;
  im.width  = m_sourceWidth;

  for (int f=0; f<3; ++f)
  {
    YUVPLANE  &plane  = fields[f][0];

    plane.texwidth  = im.width;
    plane.texheight = im.height;
    plane.pixpertex_x = 1;
    plane.pixpertex_y = 1;


    if(m_renderMethod & RENDER_POT)
    {
      plane.texwidth  = NP2(plane.texwidth);
      plane.texheight = NP2(plane.texheight);
    }
  }

  return true;
}

void CRendererRKCodec::DeleteTexture(int index)
{
#ifdef DEBUG_VERBOSE
  CLog::Log(LOGNOTICE, "%s#%d, R#%d, mediacodec %p", __FUNCTION__,__LINE__, index, m_buffers[index].hwDec );
#endif

  CDVDMediaCodecInfo *mci = static_cast<CDVDMediaCodecInfo *>(m_buffers[index].hwDec);
  SAFE_RELEASE(mci);
  m_buffers[index].hwDec = NULL;
}

#endif
