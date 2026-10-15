#pragma once

#include <QImage>
#include <QString>
#include <QVector>

// Writing an animation out: animated GIF, animated PNG, a video (through
// ffmpeg, when it is installed) or a numbered PNG for every frame.
namespace animio {

struct Frame {
    QImage image;   // the whole canvas, as the frame plays
    int hold = 1;   // how many frames' time it stays up
};

// Every format takes the frames in the order they play (a ping-pong already
// unrolled) at `fps` frames a second; `loop` plays it forever, otherwise once.
bool writeGif(const QString& path, const QVector<Frame>& frames, int fps, bool loop, QString* error);
bool writeApng(const QString& path, const QVector<Frame>& frames, int fps, bool loop, QString* error);
// MP4 (H.264) through ffmpeg; a frame held for three plays three times.
bool writeVideo(const QString& path, const QVector<Frame>& frames, int fps, QString* error);
// "walk.png" becomes walk_0001.png, walk_0002.png, ... one for every frame's
// time, so that the holds stay in the sequence. Returns how many it wrote.
int writeSequence(const QString& path, const QVector<Frame>& frames, QString* error);

QString ffmpegPath();   // empty when there is none

} // namespace animio
