// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

part of dart.io;

class _SocketBase {
  int _fd = -1;

  /**
   * Close the socket. Operations on the socket are invalid after a call to
   * [close].
   */
  void close() {
    if (_fd != -1) {
      sys.close(_fd);
      _fd = -1;
    }
  }

  void _error(String message) {
    close();
    throw new SocketException(message, sys.errno());
  }
}

class Socket extends _SocketBase {
  /**
   * Connect to the endpoint '[host]:[port]'.
   */
  Socket.connect(String host, int port) {
    var address = sys.lookup(host);
    if (address == null) _error("Failed to lookup address '$host'");
    _fd = sys.socket();
    if (_fd == -1) _error("Failed to create socket");
    sys.setBlocking(_fd, false);
    if (sys.connect(_fd, address, port) == -1 &&
        sys.errno() != Errno.EAGAIN) {
      _error("Failed to connect to $host:$port");
    }
    if (sys.addToEventHandler(_fd) == -1) {
      _error("Failed to assign socket to event handler");
    }
    int events = waitForFd(_fd, WRITE_EVENT);
    if (events != WRITE_EVENT) {
      _error("Failed to connect to $host:$port");
    }
  }

  Socket.fromFd(fd) {
    // Be sure it's not in the event handler.
    _fd = fd;
    if (sys.addToEventHandler(_fd) == -1) {
      _error("Failed to assign socket to event handler");
    }
  }

  int detachFd() {
    int fd = _fd;
    if (fd == -1 || sys.removeFromEventHandler(fd) == -1) {
      _error("Failed to detach file descriptor");
    }
    _fd = -1;
    return fd;
  }

  /**
   * Get the number of available bytes.
   */
  int get available {
    int value = sys.available(_fd);
    if (value == -1) {
      _error("Failed to get the number of available bytes");
    }
    return value;
  }

  /**
   * Read [bytes] number of bytes from the socket.
   * Will block until all bytes are available.
   * Returns `null` if the socket was closed for reading.
   */
  ByteBuffer read(int bytes) {
    var result = new ByteBuffer(bytes);
    var buffer = result;
    int offset = 0;
    while (offset < bytes) {
      int events = waitForFd(_fd, READ_EVENT);
      int read = 0;
      if ((events & READ_EVENT) != 0) {
        read = sys.read(_fd, buffer, bytes - offset);
      }
      if (read == 0 || (events & CLOSE_EVENT) != 0) {
        if (offset + read < bytes) return null;
      }
      if (read < 0 || (events & ERROR_EVENT) != 0) {
        _error("Failed to read from socket");
      }
      offset += read;
      buffer = new ByteBuffer.withOffset(buffer, offset);
    }
    return result;
  }

  /**
   * Read the next chunk of bytes.
   * Will block until some bytes are available.
   * Returns `null` if the socket was closed for reading.
   */
  ByteBuffer readNext();

  /**
   * Write [buffer] on the socket. Will block until all of [buffer] is written.
   */
  void write(ByteBuffer buffer) {
    int offset = 0;
    int bytes = buffer.length;
    while (true) {
      int wrote = sys.write(_fd, buffer, bytes - offset);
      if (wrote == -1) {
        _error("Failed to write to socket");
      }
      offset += wrote;
      if (offset == wrote) return;
      buffer = new ByteBuffer.withOffset(buffer, offset);
      int events = waitForFd(_fd, WRITE_EVENT);
      if ((events & ERROR_EVENT) != 0) {
        _error("Failed to write to socket");
      }
    }
  }
}

class ServerSocket extends _SocketBase {
  /**
   * Create a new server socket, listening on '[host]:[port]'.
   *
   * If [port] is '0', a random free port will be selected for the socket.
   */
  ServerSocket(String host, int port) {
    var address = sys.lookup(host);
    if (address == null) _error("Failed to lookup address '$host'");
    _fd = sys.socket();
    if (_fd == -1) _error("Failed to create socket");
    sys.setBlocking(_fd, false);
    if (sys.bind(_fd, address, port) == -1) {
      _error("Failed to bind to $host:$port");
    }
    if (sys.listen(_fd) == -1) _error("Failed to listen on $host:$port");
    if (sys.addToEventHandler(_fd) == -1) {
      _error("Failed to assign socket to event handler");
    }
  }

  /**
   * Get the port of the server socket.
   */
  int get port {
    int value = sys.port(_fd);
    if (value == -1) {
      _error("Failed to get port");
    }
    return value;
  }

  /**
   * Accept the incoming socket. This function will block until a socket is
   * accepted.
   */
  Socket accept() {
    int events = waitForFd(_fd, READ_EVENT);
    if (events == READ_EVENT) {
      int client = sys.accept(_fd);
      if (client == -1) _error("Failed to accept socket");
      sys.setBlocking(client, false);
      return new Socket.fromFd(client);
    }
    _error("Server socket closed while receiving socket");
  }
}

class SocketException {
  final String message;
  final Errno errno;
  SocketException(this.message, this.errno);

  String toString() => "SocketException: $message, $errno";
}
