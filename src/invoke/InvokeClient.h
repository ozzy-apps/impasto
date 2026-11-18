#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

class InvokeClient : public QObject {
    Q_OBJECT
public:
    struct Generation {
        QJsonObject model, encoder, vae;
        QString prompt, negative, scheduler;
        double strength = 0.55;
        double guidance = 4.5;
        int steps = 30;
        quint32 seed = 0;

        // A prompt for one part of the picture: where `mask` is opaque.
        struct Region {
            QString prompt, negative;
            QImage mask;   // the render size, white with the painted coverage as alpha
        };
        QVector<Region> regions;
        // A ControlNet-LLLite adapter steering the picture, from a picture of lines.
        struct Control {
            QJsonObject model;
            QImage image;  // the render size: white lines on black
            double weight = 1.0, begin = 0.0, end = 1.0;
        };
        QVector<Control> controls;
        // The LLLite inpainting adapter, when inpainting with one.
        QJsonObject inpaintAdapter;
        double inpaintAdapterWeight = 1.0;
    };

    // What the uploaded pictures are called on the server.
    struct Names {
        QString source, mask, adapterMask;
        QStringList regions, controls;
    };

    explicit InvokeClient(QObject* parent = nullptr);
    // The batch sent to Invoke's queue for these settings and uploaded pictures.
    static QJsonObject batchFor(const Generation& settings, const QSize& size, const Names& names, const QString& batchId);
    void connectTo(const QString& address, const QString& token);
    void generate(const Generation& settings, const QImage& source, const QImage& mask);
    void cancel();
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    bool canCancel() const { return m_item >= 0 && !m_canceling; }

signals:
    void stateChanged();
    void statusChanged(const QString& status);
    void modelsChanged(const QJsonArray& models);
    void imageReady(const QImage& image);

private:
    using Reply = std::function<void(const QByteArray& data, const QString& error)>;
    using Uploaded = std::function<void(const QString& name)>;

    QNetworkRequest request(const QString& path, const QUrlQuery& query = {}) const;
    void read(QNetworkReply* reply, Reply callback);
    void upload(const QImage& image, bool mask, Uploaded callback);
    void uploadNext(const Generation& settings, QVector<QPair<QImage, bool>> pending, QStringList names);
    void enqueue(const Generation& settings, const QSize& size, const Names& names);
    void poll();
    void finish(const QString& status, const QImage& result = {});
    QString itemPath() const;

    QNetworkAccessManager m_network;
    QTimer m_poll;
    QUrl m_server;
    QByteArray m_auth;
    QString m_version;
    QString m_job;
    qint64 m_item = -1;
    int m_pollErrors = 0;
    quint64 m_pollSerial = 0;
    bool m_connected = false;
    bool m_busy = false;
    bool m_canceling = false;
    bool m_hasMask = false, m_hasAdapterMask = false;
    int m_uploadCount = 0;
    QSize m_renderSize;
};
