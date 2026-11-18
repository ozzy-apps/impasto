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
    };

    explicit InvokeClient(QObject* parent = nullptr);
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
    void enqueue(const Generation& settings, const QSize& size, const QString& source, const QString& mask);
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
};
