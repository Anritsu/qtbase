// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
#define BUILD_LIBRARY
#include <qstring.h>
#include <qthread.h>
#include <stdio.h>
#include <qjsondocument.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qfileinfo.h>
#include <qrect.h>
#include <qsize.h>
#include <qmetaobject.h>
#include <qendian.h>
#include "qctflib_p.h"
#include <filesystem>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

Q_LOGGING_CATEGORY(lcDebugTrace, "qt.core.ctf", QtWarningMsg)

static const size_t packetHeaderSize = 24 + 6 * 8 + 4;
static const size_t packetSize = 4096;

static const char traceMetadataTemplate[] =
#include "metadata_template.h"
;
static const size_t traceMetadataSize = sizeof(traceMetadataTemplate);

template <typename T>
QByteArray &operator<<(QByteArray &arr, T val)
{
    static_assert(std::is_arithmetic_v<T>);
    arr.append((char *)&val, sizeof(val));
    return arr;
}

QCtfLibImpl *QCtfLibImpl::s_instance = nullptr;

QCtfLib *QCtfLibImpl::instance()
{
    if (!s_instance)
        s_instance = new QCtfLibImpl();
    return s_instance;
}

void QCtfLibImpl::cleanup()
{
    if (s_instance)
        delete s_instance;
    s_instance = nullptr;
}

QCtfLibImpl::QCtfLibImpl()
{
    QString location = QString::fromUtf8(qgetenv("QTRACE_LOCATION"));
    if (location.isEmpty()) {
        qCInfo (lcDebugTrace) << "QTRACE_LOCATION not set";
        return;
    }

    // Check if the location is writable
    FILE *file = nullptr;
    file = fopen(qPrintable(location + "/metadata"_L1), "w+b");
    if (!file) {
        qCWarning (lcDebugTrace) << "Unable to write to location";
        return;
    }
    fclose(file);

    const QString filename = location + QStringLiteral("/session.json");
    file = fopen(qPrintable(filename), "rb");
    if (!file) {
        qCWarning (lcDebugTrace) << "unable to open session file: " << filename;
        m_location = location;
        m_session.tracepoints.append(QStringLiteral("all"));
        m_session.name = QStringLiteral("default");
    } else {
        fseek(file, 0, SEEK_END);
        long pos = ftell(file);
        fseek(file, 0, SEEK_SET);
        QByteArray data(pos, Qt::Uninitialized);
        long size = (long)fread(data.data(), pos, 1, file);
        fclose(file);
        if (size != 1)
            return;
        QJsonDocument json(QJsonDocument::fromJson(data));

        QJsonObject obj = json.object();
        QJsonValue value = *obj.begin();
        if (value.isNull() || !value.isArray())
            return;
        m_session.name = obj.begin().key();
        QJsonArray arr = value.toArray();
        for (auto var : arr)
            m_session.tracepoints.append(var.toString());

        m_location = location + QStringLiteral("/ust");
        std::filesystem::create_directory(qPrintable(m_location), qPrintable(location));
    }
    m_session.all = m_session.tracepoints.contains(QStringLiteral("all"));

    auto datetime = QDateTime::currentDateTime();
    QString mhn = QSysInfo::machineHostName();
    QString metadata = QString::fromUtf8(traceMetadataTemplate, traceMetadataSize);
    metadata.replace(QStringLiteral("$TRACE_UUID"), s_TraceUuid.toString(QUuid::WithoutBraces));
    metadata.replace(QStringLiteral("$ARC_BIT_WIDTH"), QString::number(Q_PROCESSOR_WORDSIZE * 8));
    metadata.replace(QStringLiteral("$SESSION_NAME"), m_session.name);
    metadata.replace(QStringLiteral("$CREATION_TIME"), datetime.toString());
    metadata.replace(QStringLiteral("$HOST_NAME"), mhn);
    metadata.replace(QStringLiteral("$CLOCK_FREQUENCY"), m_timer.isMonotonic() ? QStringLiteral("1000000000") : QStringLiteral("1000"));
    metadata.replace(QStringLiteral("$CLOCK_NAME"), m_timer.isMonotonic() ? QStringLiteral("monotonic") : QStringLiteral("system"));
    metadata.replace(QStringLiteral("$CLOCK_TYPE"), m_timer.isMonotonic() ? QStringLiteral("Monotonic clock") : QStringLiteral("System clock"));
    metadata.replace(QStringLiteral("$CLOCK_OFFSET"), QString::number(datetime.toMSecsSinceEpoch() * 1000000));
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    metadata.replace(QStringLiteral("$ENDIANNESS"), QStringLiteral("be"));
#else
    metadata.replace(QStringLiteral("$ENDIANNESS"), QStringLiteral("le"));
#endif
    writeMetadata(metadata, true);

    m_timer.start();
}

void QCtfLibImpl::writeMetadata(const QString &metadata, bool overwrite)
{
    FILE *file = nullptr;
    file = fopen(qPrintable(m_location + "/metadata"_L1), overwrite ? "w+b": "ab");
    if (!file)
        return;

    if (!overwrite)
        fputs("\n", file);

    // data contains zero at the end, hence size - 1.
    const QByteArray data = metadata.toUtf8();
    fwrite(data.data(), data.size() - 1, 1, file);
    fclose(file);
}

void QCtfLibImpl::writeCtfPacket(QCtfLibImpl::Channel &ch)
{
    FILE *file = nullptr;
    file = fopen(ch.channelName, "ab");
    if (file) {
        /*  Each packet contains header and context, which are defined in the metadata.txt */
        QByteArray packet;
        packet << s_CtfHeaderMagic;
        /* Uuid is array of bytes hence implicitely big endian.  */
        packet << qToBigEndian(s_TraceUuid.data1);
        packet << qToBigEndian(s_TraceUuid.data2);
        packet << qToBigEndian(s_TraceUuid.data3);
        for (int i = 0; i < 8; i++)
            packet << s_TraceUuid.data4[i];

        packet << quint32(0);
        packet << ch.minTimestamp;
        packet << ch.maxTimestamp;
        packet << quint64(ch.data.size() + packetHeaderSize + ch.threadNameLength) * 8u;
        packet << quint64(packetSize) * 8u;
        packet << ch.seqnumber++;
        packet << quint64(0);
        packet << ch.threadIndex;
        if (ch.threadName.size())
            packet.append(ch.threadName);
        packet << (char)0;

        Q_ASSERT(ch.data.size() + packetHeaderSize + ch.threadNameLength <= packetSize);
        Q_ASSERT(packet.size() == qsizetype(packetHeaderSize + ch.threadNameLength));
        fwrite(packet.data(), packet.size(), 1, file);
        ch.data.resize(packetSize - packet.size(), 0);
        fwrite(ch.data.data(), ch.data.size(), 1, file);
        fclose(file);
    }
}

QCtfLibImpl::~QCtfLibImpl()
{
    qDeleteAll(m_eventPrivs);
}

bool QCtfLibImpl::tracepointEnabled(const QCtfTracePointEvent &point)
{
    return m_session.all || m_session.tracepoints.contains(point.provider.provider);
}

QCtfLibImpl::Channel::~Channel()
{
    if (data.size())
        QCtfLibImpl::writeCtfPacket(*this);
}

static QString toMetadata(const QString &provider, const QString &name, const QString &metadata, quint32 eventId)
{
/*
    generates event structure:
event {
    name = provider:tracepoint_name;
    id = eventId;
    stream_id = 0;
    loglevel = 13;
    fields := struct {
        metadata
    };
};
*/
    QString ret;
    ret  = QStringLiteral("event {\n    name = \"") + provider + QLatin1Char(':') + name + QStringLiteral("\";\n");
    ret += QStringLiteral("    id = ") + QString::number(eventId) + QStringLiteral(";\n");
    ret += QStringLiteral("    stream_id = 0;\n    loglevel = 13;\n    fields := struct {\n        ");
    ret += metadata + QStringLiteral("\n    };\n};\n");
    return ret;
}

QCtfTracePointPrivate *QCtfLibImpl::initializeTracepoint(const QCtfTracePointEvent &point)
{
    QMutexLocker lock(&m_mutex);
    QCtfTracePointPrivate *priv = point.d;
    if (!point.d) {
        if (const auto &it = m_eventPrivs.find(point.eventName); it != m_eventPrivs.end()) {
            priv = *it;
        } else {
            priv = new QCtfTracePointPrivate();
            m_eventPrivs.insert(point.eventName, priv);
            priv->id = eventId();
            priv->metadata = toMetadata(point.provider.provider, point.eventName, point.metadata, priv->id);
        }
    }
    return priv;
}

void QCtfLibImpl::doTracepoint(const QCtfTracePointEvent &point, const QByteArray &arr)
{
    QCtfTracePointPrivate *priv = point.d;
    quint64 timestamp = 0;
    QThread *thread = nullptr;
    {
        QMutexLocker lock(&m_mutex);
        if (!priv->metadataWritten) {
            priv->metadataWritten = true;
            auto providerMetadata = point.provider.metadata;
            while (providerMetadata) {
                registerMetadata(*providerMetadata);
                providerMetadata = providerMetadata->next;
            }
            if (m_newAdditionalMetadata.size()) {
                for (const QString &name : m_newAdditionalMetadata)
                    writeMetadata(m_additionalMetadata[name]->metadata);
                m_newAdditionalMetadata.clear();
            }
            writeMetadata(priv->metadata);
        }
        timestamp = m_timer.nsecsElapsed();
    }
    if (arr.size() != point.size) {
        if (arr.size() < point.size)
            return;
        if (arr.size() > point.size && !point.variableSize && !point.metadata.isEmpty())
            return;
    }

    thread = QThread::currentThread();
    if (thread == nullptr)
        return;

    Channel &ch = m_threadData.localData();

    if (ch.channelName[0] == 0) {
        m_threadIndices.insert(thread, m_threadIndices.size());
        sprintf(ch.channelName, "%s/channel_%d", qPrintable(m_location), m_threadIndices[thread]);
        FILE *f = nullptr;
        f = fopen(ch.channelName, "wb");
        if (f)
            fclose(f);
        ch.minTimestamp = ch.maxTimestamp = timestamp;
        ch.thread = thread;
        ch.threadIndex = m_threadIndices[thread];
        ch.threadName = thread->objectName().toUtf8();
        if (ch.threadName.isEmpty()) {
            const QMetaObject *obj = thread->metaObject();
            ch.threadName = QByteArray(obj->className());
        }
        ch.threadNameLength = ch.threadName.size() + 1;
    }
    if (ch.locked)
        return;
    Q_ASSERT(ch.thread == thread);
    ch.locked = true;

    QByteArray event;
    event << priv->id << timestamp;
    if (!point.metadata.isEmpty())
        event.append(arr);

    if (ch.threadNameLength + ch.data.size() + event.size() + packetHeaderSize >= packetSize) {
        writeCtfPacket(ch);
        ch.data = event;
        ch.minTimestamp = ch.maxTimestamp = timestamp;
    } else {
        ch.data.append(event);
    }

    ch.locked = false;
    ch.maxTimestamp = timestamp;
}

bool QCtfLibImpl::sessionEnabled()
{
    return !m_session.name.isEmpty();
}

int QCtfLibImpl::eventId()
{
    return m_eventId++;
}

void QCtfLibImpl::registerMetadata(const QCtfTraceMetadata &metadata)
{
    if (m_additionalMetadata.contains(metadata.name))
        return;

    m_additionalMetadata.insert(metadata.name, &metadata);
    m_newAdditionalMetadata.insert(metadata.name);
}

QT_END_NAMESPACE
