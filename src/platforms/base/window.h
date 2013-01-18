// This file is part of QtUbuntu, a set of Qt components for Ubuntu.
// Copyright © 2013 Canonical Ltd.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation; version 3.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef QUBUNTUBASEWINDOW_H
#define QUBUNTUBASEWINDOW_H

#include <qpa/qplatformwindow.h>
#include <EGL/egl.h>

class QUbuntuBaseScreen;

class QUbuntuBaseWindow : public QPlatformWindow {
 public:
  QUbuntuBaseWindow(QWindow* w, QUbuntuBaseScreen* screen);
  ~QUbuntuBaseWindow();

  // QPlatformWindow methods.
  WId winId() const { return id_; }

  // New methods.
  void createSurface(EGLNativeWindowType nativeWindow);
  EGLSurface eglSurface() const { return eglSurface_; }

 private:
  QUbuntuBaseScreen* screen_;
  EGLSurface eglSurface_;
  WId id_;
};

#endif  // QUBUNTUBASEWINDOW_H
