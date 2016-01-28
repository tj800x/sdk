// Copyright (c) 2015, the Dartino project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.
//

import 'package:lk/framebuffer.dart';

class MovablePoint {
  final int x, y;
  int mx, my;

  MovablePoint(this.x, this.y, this.mx, this.my);

  getNextPoint(FrameBuffer surface) {
    int newX = x + mx;
    int newMX = mx;
    if (newX < 0 || newX > surface.width) {
      newMX = -mx;
      newX = x + newMX;
    }
    int newY = y + my;
    int newMY = my;
    if (newY < 0 || newY > surface.height) {
      newMY = -my;
      newY = y + newMY;
    }

    return new MovablePoint(newX, newY, newMX, newMY);
  }
}

class MovableLine {
  MovablePoint a, b;
  final int color;

  MovableLine(this.a, this.b, this.color);

  getNextLine(FrameBuffer surface) {
    return new MovableLine(a.getNextPoint(surface), b.getNextPoint(surface),
        color);
  }

  draw(FrameBuffer surface) {
    surface.drawLine(a.x, a.y, b.x, b.y, color);
  }

  erase(FrameBuffer surface) {
    surface.drawLine(a.x, a.y, b.x, b.y, 0xFF000000);
  }
}

/*
 * The below code is on purpose written to create a lot of garbage to test
 * whether we can get a smooth animation even in the presence of lots of
 * GC.
 */
main () {
  List lines = [];

  lines.add(new MovableLine(new MovablePoint(1,1,2,3),
      new MovablePoint(10, 10, 3, 2), 0xFFFF00FF));
  lines.add(new MovableLine(new MovablePoint(1,1,5,-2),
      new MovablePoint(10, 10, -2, 5), 0xFF0000FF));
  lines.add(new MovableLine(new MovablePoint(1,1,9,4),
      new MovablePoint(10, 10, 3, 8), 0xFFFF0000));
  lines.add(new MovableLine(new MovablePoint(1,1,-1,2),
      new MovablePoint(10, 10, 2, -1), 0xFF77FF33));
  lines.add(new MovableLine(new MovablePoint(1,1,1,-2),
      new MovablePoint(10, 20, -2, 1), 0xFF0033FF));
  lines.add(new MovableLine(new MovablePoint(1,1,-9,3),
      new MovablePoint(20, 10, 6, 8), 0xFF773399));
  lines.add(new MovableLine(new MovablePoint(1,1,7,-2),
      new MovablePoint(10, 1, -2, -3), 0xFF77FF00));

  List history = [lines];
  var surface = new FrameBuffer();

  while (true) {
    List next = history.first.map((line) => line.getNextLine(surface)).toList();
    next.forEach((line) => line.draw(surface));
    history.insert(0, next);
    if (history.length > 10) {
      history.removeLast().forEach((line) => line.erase(surface));
    }
  }
}
