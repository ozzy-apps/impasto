#include "invoke/InvokeClient.h"

#include <QBuffer>
#include <QHttpMultiPart>
#include <QImageReader>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUuid>

namespace {

constexpr int pollIntervalMs = 1300;

QJsonObject parseObject(const QByteArray& data)
{
    return QJsonDocument::fromJson(data).object();
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject imageField(const QString& name)
{
    return {{QStringLiteral("image_name"), name}};
}

// Invoke's model list can include server-side paths. Send only the fields
// expected by ModelIdentifierField when referencing a registered model.
QJsonObject modelField(const QJsonObject& model)
{
    QJsonObject result;
    for (const QString& key : {QStringLiteral("key"), QStringLiteral("hash"), QStringLiteral("name"),
                               QStringLiteral("base"), QStringLiteral("type")})
        result.insert(key, model.value(key));
    return result;
}

QJsonObject makeBatch(const InvokeClient::Generation& g, const QSize& size, const InvokeClient::Names& names, const QString& batchId)
{
    QJsonObject nodes;
    QJsonArray edges;
    auto node = [&](const QString& id, const QString& type, QJsonObject fields = {}, bool intermediate = true) {
        fields.insert(QStringLiteral("id"), id);
        fields.insert(QStringLiteral("type"), type);
        fields.insert(QStringLiteral("is_intermediate"), intermediate);
        nodes.insert(id, fields);
    };
    auto edge = [&](const QString& from, const QString& output, const QString& to, const QString& input) {
        edges.append(QJsonObject{{QStringLiteral("source"), QJsonObject{{QStringLiteral("node_id"), from},
                                                                         {QStringLiteral("field"), output}}},
                                 {QStringLiteral("destination"), QJsonObject{{QStringLiteral("node_id"), to},
                                                                              {QStringLiteral("field"), input}}}});
    };
    node("model", "anima_model_loader", {{"model", modelField(g.model)}, {"vae_model", modelField(g.vae)},
                                         {"qwen3_encoder_model", modelField(g.encoder)}});
    node("encode", "anima_i2l", {{"image", imageField(names.source)}});
    node("denoise", "anima_denoise", {{"width", size.width()}, {"height", size.height()},
                                      {"seed", qint64(g.seed)}, {"steps", g.steps}, {"guidance_scale", g.guidance},
                                      {"denoising_start", 1.0 - g.strength}, {"scheduler", g.scheduler}});
    node("decode", "anima_l2i", {}, false);
    edge("model", "vae", "encode", "vae");
    edge("model", "vae", "decode", "vae");
    edge("model", "transformer", "denoise", "transformer");
    edge("encode", "latents", "denoise", "latents");
    edge("denoise", "latents", "decode", "latents");

    // Prompts go through a collection, which regional prompts join, as in Invoke's own canvas.
    auto conditioning = [&](const QString& id, const QString& prompt) {
        const QString collection = id + QStringLiteral("_collection");
        node(id, "anima_text_encoder", {{"prompt", prompt}});
        node(collection, "collect");
        edge("model", "qwen3_encoder", id, "qwen3_encoder");
        edge(id, "conditioning", collection, "item");
        edge(collection, "collection", "denoise", id + QStringLiteral("_conditioning"));
    };
    const bool negative = g.guidance > 1.0;
    conditioning("positive", g.prompt);
    if (negative)
        conditioning("negative", g.negative);
    for (int i = 0; i < g.regions.size() && i < names.regions.size(); ++i) {
        const auto& region = g.regions[i];
        const QString mask = QStringLiteral("region_mask_%1").arg(i);
        node(mask, "alpha_mask_to_tensor", {{"image", imageField(names.regions[i])}});
        auto regional = [&](const QString& id, const QString& prompt, const QString& collection) {
            node(id, "anima_text_encoder", {{"prompt", prompt}});
            edge("model", "qwen3_encoder", id, "qwen3_encoder");
            edge(mask, "mask", id, "mask");
            edge(id, "conditioning", collection, "item");
        };
        if (!region.prompt.isEmpty())
            regional(QStringLiteral("region_positive_%1").arg(i), region.prompt, QStringLiteral("positive_collection"));
        if (negative && !region.negative.isEmpty())
            regional(QStringLiteral("region_negative_%1").arg(i), region.negative, QStringLiteral("negative_collection"));
    }

    if (!names.mask.isEmpty()) {
        // No image connection here. Anima consumes the denoise mask directly,
        // without the masked latent encoding used by specialized SD models.
        node("mask", "create_denoise_mask", {{"mask", imageField(names.mask)}});
        edge("model", "vae", "mask", "vae");
        edge("mask", "denoise_mask", "denoise", "denoise_mask");
    }

    // ControlNet-LLLite adapters, the control layers and the inpainting one, share one collection.
    bool lllite = false;
    auto adapter = [&](const QString& id, QJsonObject fields) {
        if (!lllite)
            node("lllite_collection", "collect");
        lllite = true;
        node(id, "anima_lllite", fields);
        edge(id, "control", "lllite_collection", "item");
    };
    for (int i = 0; i < g.controls.size() && i < names.controls.size(); ++i) {
        const auto& c = g.controls[i];
        adapter(QStringLiteral("control_%1").arg(i), {{"image", imageField(names.controls[i])}, {"control_model", modelField(c.model)},
                                                      {"weight", c.weight}, {"begin_step_percent", c.begin}, {"end_step_percent", c.end}});
    }
    if (!g.inpaintAdapter.isEmpty() && !names.adapterMask.isEmpty())
        adapter(QStringLiteral("inpaint_adapter"), {{"image", imageField(names.source)}, {"mask", imageField(names.adapterMask)},
                                                    {"control_model", modelField(g.inpaintAdapter)}, {"weight", g.inpaintAdapterWeight}});
    if (lllite)
        edge("lllite_collection", "collection", "denoise", "control_lllite");

    return {{"batch", QJsonObject{{"batch_id", batchId}, {"origin", "impasto"}, {"runs", 1},
                                  {"graph", QJsonObject{{"id", batchId}, {"nodes", nodes}, {"edges", edges}}}}}};
}

} // namespace

QJsonObject InvokeClient::batchFor(const Generation& settings, const QSize& size, const Names& names, const QString& batchId)
{
    return makeBatch(settings, size, names, batchId);
}

InvokeClient::InvokeClient(QObject* parent) : QObject(parent)
{
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &InvokeClient::poll);
}

QNetworkRequest InvokeClient::request(const QString& path, const QUrlQuery& query) const
{
    QUrl url = m_server;
    url.setPath(m_server.path() + path);
    url.setQuery(query);
    QNetworkRequest req(url);
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!m_auth.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + m_auth);
    return req;
}

void InvokeClient::read(QNetworkReply* reply, Reply callback)
{
    connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
        const QByteArray data = reply->readAll();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error;
        if (code >= 300 && code < 400) {
            error = tr("The server redirected this request. Enter its final URL.");
        } else if (reply->error() != QNetworkReply::NoError) {
            const QJsonValue detail = parseObject(data).value(QStringLiteral("detail"));
            if (detail.isString())
                error = detail.toString();
            else if (detail.isArray() && !detail.toArray().isEmpty()) {
                const QJsonObject issue = detail.toArray().first().toObject();
                const QJsonArray location = issue.value(QStringLiteral("loc")).toArray();
                QStringList fields;
                for (const QJsonValue& field : location)
                    fields.append(field.toVariant().toString());
                error = tr("%1 (%2)").arg(issue.value(QStringLiteral("msg")).toString(), fields.join(QLatin1Char('.')));
            }
            if (error.isEmpty())
                error = reply->errorString();
            if (code >= 400)
                error = tr("HTTP %1 · %2").arg(code).arg(error);
        }
        reply->deleteLater();
        callback(data, error.left(400));
    });
}

void InvokeClient::connectTo(const QString& address, const QString& token)
{
    if (m_busy)
        return;
    QString text = address.trimmed();
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("http://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        m_connected = false;
        m_auth.clear();
        emit modelsChanged({});
        finish(tr("Enter an HTTP or HTTPS server URL, such as http://127.0.0.1:9090."));
        return;
    }

    m_server = url.adjusted(QUrl::StripTrailingSlash);
    m_auth = token.trimmed().toUtf8();
    m_connected = false;
    m_busy = true;
    emit modelsChanged({});
    emit stateChanged();
    emit statusChanged(tr("Connecting…"));
    read(m_network.get(request(QStringLiteral("/api/v1/app/version"))), [this](const QByteArray& data, const QString& error) {
        if (!error.isEmpty()) {
            finish(tr("Connection failed · %1").arg(error));
            return;
        }
        m_version = parseObject(data).value(QStringLiteral("version")).toString();
        if (m_version.isEmpty()) {
            finish(tr("This address did not return an InvokeAI version."));
            return;
        }
        read(m_network.get(request(QStringLiteral("/api/v2/models/"))), [this](const QByteArray& body, const QString& modelError) {
            const QJsonValue models = parseObject(body).value(QStringLiteral("models"));
            if (!modelError.isEmpty() || !models.isArray()) {
                finish(tr("Could not load Invoke models · %1").arg(modelError.isEmpty() ? tr("Unexpected response") : modelError));
                return;
            }
            m_connected = true;
            emit modelsChanged(models.toArray());
            finish(tr("InvokeAI %1 connected").arg(m_version));
        });
    });
}

void InvokeClient::generate(const Generation& settings, const QImage& source, const QImage& mask)
{
    if (!m_connected || m_busy || source.isNull())
        return;
    m_job = newId();
    m_busy = true;
    m_pollErrors = 0;
    emit stateChanged();
    // Every picture the graph reads, in order: the canvas, the inpaint mask and
    // the adapter's copy of it (white = inpaint), each region's mask, each control.
    QVector<QPair<QImage, bool>> pending{{source, false}};
    m_renderSize = source.size();
    m_hasMask = !mask.isNull();
    m_hasAdapterMask = m_hasMask && !settings.inpaintAdapter.isEmpty();
    if (m_hasMask)
        pending.push_back({mask, true});
    if (m_hasAdapterMask) {
        QImage inpaintArea = mask.convertToFormat(QImage::Format_Grayscale8);
        inpaintArea.invertPixels();
        pending.push_back({inpaintArea, true});
    }
    for (const auto& region : settings.regions)
        pending.push_back({region.mask, true});
    for (const auto& control : settings.controls)
        pending.push_back({control.image, false});
    m_uploadCount = int(pending.size());
    uploadNext(settings, pending, {});
}

void InvokeClient::uploadNext(const Generation& settings, QVector<QPair<QImage, bool>> pending, QStringList names)
{
    if (pending.isEmpty()) {
        Names n;
        int at = 0;
        n.source = names.value(at++);
        if (m_hasMask)
            n.mask = names.value(at++);
        if (m_hasAdapterMask)
            n.adapterMask = names.value(at++);
        for (int i = 0; i < settings.regions.size(); ++i)
            n.regions.append(names.value(at++));
        for (int i = 0; i < settings.controls.size(); ++i)
            n.controls.append(names.value(at++));
        enqueue(settings, m_renderSize, n);
        return;
    }
    const int done = int(names.size());
    emit statusChanged(m_uploadCount > 1 ? tr("Uploading %1 of %2…").arg(done + 1).arg(m_uploadCount) : tr("Uploading canvas…"));
    const auto [image, isMask] = pending.takeFirst();
    upload(image, isMask, [this, settings, pending, names](const QString& name) mutable {
        names.append(name);
        uploadNext(settings, pending, names);
    });
}

void InvokeClient::upload(const QImage& image, bool mask, Uploaded callback)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        finish(tr("Could not encode the generation input."));
        return;
    }
    auto* body = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"file\"; filename=\"impasto.png\""));
    part.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
    part.setBody(png);
    body->append(part);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("image_category"), mask ? QStringLiteral("mask") : QStringLiteral("user"));
    query.addQueryItem(QStringLiteral("is_intermediate"), QStringLiteral("true"));
    auto* reply = m_network.post(request(QStringLiteral("/api/v1/images/upload"), query), body);
    body->setParent(reply);
    read(reply, [this, callback = std::move(callback)](const QByteArray& data, const QString& error) {
        const QString name = parseObject(data).value(QStringLiteral("image_name")).toString();
        if (!error.isEmpty() || name.isEmpty()) {
            finish(tr("Upload failed · %1").arg(error.isEmpty() ? tr("Missing image name") : error));
            return;
        }
        callback(name);
    });
}

void InvokeClient::enqueue(const Generation& settings, const QSize& size, const Names& names)
{
    emit statusChanged(tr("Submitting to Invoke…"));
    auto req = request(QStringLiteral("/api/v1/queue/default/enqueue_batch"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray json = QJsonDocument(makeBatch(settings, size, names, m_job)).toJson(QJsonDocument::Compact);
    read(m_network.post(req, json), [this](const QByteArray& data, const QString& error) {
        const auto ids = parseObject(data).value(QStringLiteral("item_ids")).toArray();
        m_item = ids.isEmpty() ? -1 : ids.first().toInteger(-1);
        if (!error.isEmpty() || m_item < 0) {
            finish(tr("Queue submission failed · %1").arg(error.isEmpty() ? tr("No job was queued") : error));
            return;
        }
        emit stateChanged();
        emit statusChanged(tr("Queued…"));
        m_poll.start(pollIntervalMs);
    });
}

QString InvokeClient::itemPath() const
{
    return QStringLiteral("/api/v1/queue/default/i/%1").arg(m_item);
}

void InvokeClient::poll()
{
    const quint64 serial = ++m_pollSerial;
    read(m_network.get(request(itemPath())), [this, serial](const QByteArray& data, const QString& error) {
        if (serial != m_pollSerial)
            return;
        if (!error.isEmpty()) {
            if (++m_pollErrors < 4) {
                emit statusChanged(tr("Reconnecting to the queued job…"));
                m_poll.start(2500);
            } else {
                finish(tr("Lost contact with Invoke job %1 · %2").arg(m_item).arg(error));
            }
            return;
        }
        m_pollErrors = 0;
        const QJsonObject item = parseObject(data);
        const QString status = item.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("pending") || status == QLatin1String("in_progress")) {
            emit statusChanged(status == QLatin1String("pending") ? tr("Queued…") : tr("Generating…"));
            m_poll.start(pollIntervalMs);
            return;
        }
        if (status != QLatin1String("completed")) {
            if (status == QLatin1String("canceled"))
                finish(tr("Generation canceled."));
            else if (status == QLatin1String("failed"))
                finish(tr("Generation failed · %1").arg(item.value(QStringLiteral("error_message")).toString().left(400)));
            else
                finish(tr("Invoke returned an unexpected queue status."));
            return;
        }

        const QJsonObject results = item.value(QStringLiteral("session")).toObject().value(QStringLiteral("results")).toObject();
        QString name;
        for (const auto& value : results) {
            const QJsonObject result = value.toObject();
            if (result.value(QStringLiteral("type")) == QLatin1String("image_output"))
                name = result.value(QStringLiteral("image")).toObject().value(QStringLiteral("image_name")).toString();
        }
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
            name == QLatin1String(".") || name == QLatin1String("..")) {
            finish(tr("Invoke completed without a usable image reference."));
            return;
        }

        m_item = -1;
        emit stateChanged();
        emit statusChanged(tr("Downloading result…"));
        read(m_network.get(request(QStringLiteral("/api/v1/images/i/") + name + QStringLiteral("/full"))),
             [this](const QByteArray& imageData, const QString& imageError) {
            QBuffer imageBuffer;
            imageBuffer.setData(imageData);
            imageBuffer.open(QIODevice::ReadOnly);
            QImageReader reader(&imageBuffer);
            const QSize size = reader.size();
            const QImage image = imageError.isEmpty() && size.isValid() && size.width() <= 4096 && size.height() <= 4096
                                     ? reader.read() : QImage();
            if (image.isNull()) {
                finish(tr("Could not download the result · %1").arg(imageError.isEmpty() ? tr("Invalid image") : imageError));
                return;
            }
            finish(tr("Result ready."), image);
        });
    });
}

void InvokeClient::cancel()
{
    if (!canCancel())
        return;
    m_canceling = true;
    ++m_pollSerial;
    m_poll.stop();
    emit stateChanged();
    emit statusChanged(tr("Canceling…"));
    read(m_network.put(request(itemPath() + QStringLiteral("/cancel")), QByteArray()),
         [this](const QByteArray&, const QString& error) {
        m_canceling = false;
        emit stateChanged();
        if (!error.isEmpty())
            emit statusChanged(tr("Could not cancel · %1").arg(error));
        m_poll.start(error.isEmpty() ? 0 : pollIntervalMs);
    });
}

void InvokeClient::finish(const QString& status, const QImage& result)
{
    ++m_pollSerial;
    m_poll.stop();
    m_job.clear();
    m_item = -1;
    m_busy = false;
    m_canceling = false;
    emit statusChanged(status);
    if (!result.isNull())
        emit imageReady(result);
    emit stateChanged();
}
