/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2017 Pierre Ossman for Cendio AB
 * Copyright 2014 Brian P. Hinz
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */
//
// XserverDesktop.cxx
//

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/utsname.h>

#include <network/Socket.h>
#include <rfb/Exception.h>
#include <rfb/VNCServerST.h>
#include <rfb/LogWriter.h>
#include <rfb/Configuration.h>
#include <rfb/ServerCore.h>

#include "XserverDesktop.h"
#include "vncBlockHandler.h"
#include "vncExtInit.h"
#include "vncHooks.h"
#include "vncSelection.h"
#include "XorgGlue.h"
#include "Input.h"

extern "C" {
void vncSetGlueContext(int screenIndex);
}

using namespace rfb;
using namespace network;

static LogWriter vlog("XserverDesktop");

BoolParameter rawKeyboard("RawKeyboard",
                          "Send keyboard events straight through and "
                          "avoid mapping them to the current keyboard "
                          "layout", false);
IntParameter queryConnectTimeout("QueryConnectTimeout",
                                 "Number of seconds to show the "
                                 "Accept Connection dialog before "
                                 "rejecting the connection",
                                 10);

XserverDesktop::XserverDesktop(int screenIndex_,
                               std::list<network::SocketListener*> listeners_,
                               const char* name, const rfb::PixelFormat &pf,
                               int width, int height,
                               void* fbptr, int stride)
  : screenIndex(screenIndex_),
    server(0), listeners(listeners_),
    directFbptr(true),
    queryConnectId(0), queryConnectTimer(this), resizing(false)
{
  format = pf;

  server = new VNCServerST(name, this);
  setFramebuffer(width, height, fbptr, stride);
  server->setQueryConnectionHandler(this);

  for (std::list<SocketListener*>::iterator i = listeners.begin();
       i != listeners.end();
       i++) {
    vncSetNotifyFd((*i)->getFd(), screenIndex, true, false);
    if ((*i)->getMessager())
      server->setAPIMessager((*i)->getMessager());
  }
}

XserverDesktop::~XserverDesktop()
{
  while (!listeners.empty()) {
    vncRemoveNotifyFd(listeners.back()->getFd());
    delete listeners.back();
    listeners.pop_back();
  }
  if (!directFbptr)
    delete [] data;
  delete server;
}

void XserverDesktop::blockUpdates()
{
  server->blockUpdates();
}

void XserverDesktop::unblockUpdates()
{
  server->unblockUpdates();
}

void XserverDesktop::setFramebuffer(int w, int h, void* fbptr, int stride_)
{
  ScreenSet layout;

  width_ = w;
  height_ = h;

  if (!directFbptr) {
    delete [] data;
    directFbptr = true;
  }

  if (!fbptr) {
    fbptr = new rdr::U8[w * h * (format.bpp/8)];
    stride_ = w;
    directFbptr = false;
  }

  data = (rdr::U8*)fbptr;
  stride = stride_;

  vncSetGlueContext(screenIndex);
  layout = ::computeScreenLayout(&outputIdMap);

  server->setPixelBuffer(this, layout);
}

void XserverDesktop::refreshScreenLayout()
{
  vncSetGlueContext(screenIndex);
  server->setScreenLayout(::computeScreenLayout(&outputIdMap));
}

rfb::VNCServerST::queryResult
XserverDesktop::queryConnection(network::Socket* sock,
                                const char* userName,
                                char** reason)
{
  int count;

  if (queryConnectTimer.isStarted()) {
    *reason = strDup("Another connection is currently being queried.");
    return rfb::VNCServerST::REJECT;
  }

  count = vncNotifyQueryConnect();
  if (count == 0) {
    *reason = strDup("Unable to query the local user to accept the connection.");
    return rfb::VNCServerST::REJECT;
  }

  queryConnectAddress.replaceBuf(sock->getPeerAddress());
  if (!userName)
    userName = "(anonymous)";
  queryConnectUsername.replaceBuf(strDup(userName));
  queryConnectId = (uint32_t)(intptr_t)sock;
  queryConnectSocket = sock;

  queryConnectTimer.start(queryConnectTimeout * 1000);

  return rfb::VNCServerST::PENDING;
}

void XserverDesktop::announceClipboard(bool available)
{
  try {
    server->announceClipboard(available);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::announceClipboard: %s",e.str());
  }
}

void XserverDesktop::clearBinaryClipboardData()
{
  try {
    server->clearBinaryClipboardData();
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::clearBinaryClipboardData: %s",e.str());
  }
}

void XserverDesktop::sendBinaryClipboardData(const char* mime,
                                             const unsigned char *data,
                                             const unsigned len)
{
  try {
    server->sendBinaryClipboardData(mime, data, len);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::sendBinaryClipboardData: %s",e.str());
  }
}

void XserverDesktop::getBinaryClipboardData(const char* mime,
                                            const unsigned char **data,
                                            unsigned *len)
{
  try {
    server->getBinaryClipboardData(mime, data, len);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::getBinaryClipboardData: %s",e.str());
  }
}

void XserverDesktop::bell()
{
  server->bell();
}

void XserverDesktop::setLEDState(unsigned int state)
{
  server->setLEDState(state);
}

void XserverDesktop::setDesktopName(const char* name)
{
  try {
    server->setName(name);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::setDesktopName: %s",e.str());
  }
}

void XserverDesktop::setCursor(int width, int height, int hotX, int hotY,
                               const unsigned char *rgbaData)
{
  rdr::U8* cursorData;

  rdr::U8 *out;
  const unsigned char *in;

  cursorData = new rdr::U8[width * height * 4];

  // Un-premultiply alpha
  in = rgbaData;
  out = cursorData;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      rdr::U8 alpha;

      alpha = in[3];
      if (alpha == 0)
        alpha = 1; // Avoid division by zero

      *out++ = (unsigned)*in++ * 255/alpha;
      *out++ = (unsigned)*in++ * 255/alpha;
      *out++ = (unsigned)*in++ * 255/alpha;
      *out++ = *in++;
    }
  }

  try {
    server->setCursor(width, height, Point(hotX, hotY), cursorData, resizing);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::setCursor: %s",e.str());
  }

  delete [] cursorData;
}

void XserverDesktop::setCursorPos(int x, int y, bool warped)
{
  try {
    server->setCursorPos(Point(x, y), warped);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::setCursorPos: %s",e.str());
  }
}

void XserverDesktop::add_changed(const rfb::Region &region)
{
  try {
    server->add_changed(region);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::add_changed: %s",e.str());
  }
}

void XserverDesktop::add_copied(const rfb::Region &dest, const rfb::Point &delta)
{
  try {
    server->add_copied(dest, delta);
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::add_copied: %s",e.str());
  }
}

void XserverDesktop::handleSocketEvent(int fd, bool read, bool write)
{
  try {
    if (read) {
      if (handleListenerEvent(fd, &listeners, server))
        return;
    }

    if (handleSocketEvent(fd, server, read, write))
      return;

    vlog.error("Cannot find file descriptor for socket event");
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::handleSocketEvent: %s",e.str());
  }
}

bool XserverDesktop::handleListenerEvent(int fd,
                                         std::list<SocketListener*>* sockets,
                                         SocketServer* sockserv)
{
  std::list<SocketListener*>::iterator i;

  for (i = sockets->begin(); i != sockets->end(); i++) {
    if ((*i)->getFd() == fd)
      break;
  }

  if (i == sockets->end())
    return false;

  Socket* sock = (*i)->accept();
  sock->outStream().setBlocking(false);
  vlog.debug("new client, sock %d", sock->getFd());
  sockserv->addSocket(sock);
  vncSetNotifyFd(sock->getFd(), screenIndex, true, false);

  return true;
}

bool XserverDesktop::handleSocketEvent(int fd,
                                       SocketServer* sockserv,
                                       bool read, bool write)
{
  std::list<Socket*> sockets;
  std::list<Socket*>::iterator i;

  sockserv->getSockets(&sockets);
  for (i = sockets.begin(); i != sockets.end(); i++) {
    if ((*i)->getFd() == fd)
      break;
  }

  if (i == sockets.end())
    return false;

  if (read)
    sockserv->processSocketReadEvent(*i);

  if (write)
    sockserv->processSocketWriteEvent(*i);

  return true;
}

void XserverDesktop::blockHandler(int* timeout)
{
  // We don't have a good callback for when we can init input devices[1],
  // so we abuse the fact that this routine will be called first thing
  // once the dix is done initialising.
  // [1] Technically Xvnc has InitInput(), but libvnc.so has nothing.
  vncInitInputDevice();

  try {
    std::list<Socket*> sockets;
    std::list<Socket*>::iterator i;
    server->getSockets(&sockets);
    for (i = sockets.begin(); i != sockets.end(); i++) {
      int fd = (*i)->getFd();
      if ((*i)->isShutdown()) {
        vlog.debug("client gone, sock %d",fd);
        vncRemoveNotifyFd(fd);
        server->removeSocket(*i);
        vncClientGone(fd);
        delete (*i);
      } else {
        /* Update existing NotifyFD to listen for write (or not) */
        vncSetNotifyFd(fd, screenIndex, true, (*i)->outStream().bufferUsage() > 0);
      }
    }

    // We are responsible for propagating mouse movement between clients
    int cursorX, cursorY;
    vncGetPointerPos(&cursorX, &cursorY);
    cursorX -= vncGetScreenX(screenIndex);
    cursorY -= vncGetScreenY(screenIndex);
    if (oldCursorPos.x != cursorX || oldCursorPos.y != cursorY) {
      oldCursorPos.x = cursorX;
      oldCursorPos.y = cursorY;
      server->setCursorPos(oldCursorPos, false);
    }

    // Trigger timers and check when the next will expire
    int nextTimeout = server->checkTimeouts();
    if (nextTimeout > 0 && (*timeout == -1 || nextTimeout < *timeout))
      *timeout = nextTimeout;
  } catch (rdr::Exception& e) {
    vlog.error("XserverDesktop::blockHandler: %s",e.str());
  }
}

void XserverDesktop::addClient(Socket* sock, bool reverse)
{
  vlog.debug("new client, sock %d reverse %d",sock->getFd(),reverse);
  sock->outStream().setBlocking(false);
  server->addSocket(sock, reverse);
  vncSetNotifyFd(sock->getFd(), screenIndex, true, false);
}

void XserverDesktop::disconnectClients()
{
  vlog.debug("disconnecting all clients");
  return server->closeClients("Disconnection from server end");
}


void XserverDesktop::getQueryConnect(uint32_t* opaqueId,
                                     const char** address,
                                     const char** username,
                                     int *timeout)
{
  *opaqueId = queryConnectId;

  if (!queryConnectTimer.isStarted()) {
    *address = "";
    *username = "";
    *timeout = 0;
  } else {
    *address = queryConnectAddress.buf;
    *username = queryConnectUsername.buf;
    *timeout = queryConnectTimeout;
  }
}

void XserverDesktop::approveConnection(uint32_t opaqueId, bool accept,
                                       const char* rejectMsg)
{
  if (queryConnectId == opaqueId) {
    server->approveConnection(queryConnectSocket, accept, rejectMsg);
    queryConnectId = 0;
    queryConnectTimer.stop();
  }
}

///////////////////////////////////////////////////////////////////////////
//
// SDesktop callbacks


void XserverDesktop::pointerEvent(const Point& pos, int buttonMask,
                                  const bool skipClick, const bool skipRelease, int scrollX, int scrollY)
{
  if (scrollX == 0 && scrollY == 0) {
    vncPointerMove(pos.x + vncGetScreenX(screenIndex), pos.y + vncGetScreenY(screenIndex));
    vncPointerButtonAction(buttonMask, skipClick, skipRelease);
  } else {
    vncScroll(scrollX, scrollY);
  }
}

unsigned int XserverDesktop::setScreenLayout(int fb_width, int fb_height,
                                             const rfb::ScreenSet& layout)
{
  char buffer[2048];
  vlog.debug("Got request for framebuffer resize to %dx%d",
             fb_width, fb_height);
  layout.print(buffer, sizeof(buffer));
  vlog.debug("%s", buffer);

  resizing = true;
  vncSetGlueContext(screenIndex);
  const unsigned int ret = ::setScreenLayout(fb_width, fb_height, layout, &outputIdMap);
  resizing = false;
  return ret;
}

void XserverDesktop::handleClipboardAnnounce(bool available)
{
  vncHandleClipboardAnnounce(available);
}

void XserverDesktop::handleClipboardAnnounceBinary(const unsigned num, const char mimes[][32])
{
  vncHandleClipboardAnnounceBinary(num, mimes);
}

void XserverDesktop::grabRegion(const rfb::Region& region)
{
  if (directFbptr)
    return;

  std::vector<rfb::Rect> rects;
  std::vector<rfb::Rect>::iterator i;
  region.get_rects(&rects);
  for (i = rects.begin(); i != rects.end(); i++) {
    rdr::U8 *buffer;
    int stride;

    buffer = getBufferRW(*i, &stride);
    vncGetScreenImage(screenIndex, i->tl.x, i->tl.y, i->width(), i->height(),
                      (char*)buffer, stride * format.bpp/8);
    commitBufferRW(*i);
  }
}

void XserverDesktop::keyEvent(rdr::U32 keysym, rdr::U32 keycode, bool down)
{
  if (!rawKeyboard)
    keycode = 0;

  vncKeyboardEvent(keysym, keycode, down);
}

bool XserverDesktop::handleTimeout(Timer* t)
{
  if (t == &queryConnectTimer) {
    server->approveConnection(queryConnectSocket, false,
                              "The attempt to prompt the user to "
                              "accept the connection failed");
  }

  return false;
}
