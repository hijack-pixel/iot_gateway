#include <QtWidgets>
#include <QTcpSocket>
#include <QTimer>
#include <deque>
#include <algorithm>
#include <cstdint>

static const uint8_t kMagic = 0xA5;
static const uint8_t MSG_STAT = 0x90;    // 统计帧类型

// ---- 简易实时曲线：存最近 180 个采样，QPainter 画折线 ----
class Chart : public QWidget {
public:
    explicit Chart(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(160);
    }
    void addSample(double v) {
        m_data.push_back(v);
        if (m_data.size() > 180) m_data.pop_front();
        update();                          // 触发重绘（Qt 的老规矩）
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(15, 18, 24));
        if (m_data.size() < 2) return;
        double vmax = *std::max_element(m_data.begin(), m_data.end());
        if (vmax < 1) vmax = 1;
        QPainterPath path;
        for (int i = 0; i < (int)m_data.size(); ++i) {
            double x = width() * i / (m_data.size() - 1.0);
            double y = height() - 8 - (height() - 16) * m_data[i] / vmax;
            (i == 0) ? path.moveTo(x, y) : path.lineTo(x, y);
        }
        p.setPen(QPen(QColor(80, 220, 100), 2));
        p.drawPath(path);
        p.setPen(Qt::gray);
        p.drawText(8, 16, QString("rate  max=%1").arg(vmax, 0, 'f', 0));
    }
private:
    std::deque<double> m_data;
};

// ---- 面板主窗口：TCP客户端 + 解帧 + 展示 ----
class Panel : public QWidget {
public:
    Panel() {
        auto* lay = new QVBoxLayout(this);
        m_info  = new QLabel("connecting to gateway:9001 ...");
        m_chart = new Chart;
        lay->addWidget(m_info);
        lay->addWidget(m_chart, 1);

        m_sock = new QTcpSocket(this);
        // readyRead = Qt版EPOLLIN：缓冲有数据，来收
        connect(m_sock, &QTcpSocket::readyRead, this, [this] { onReady(); });
        connect(m_sock, &QTcpSocket::disconnected, this, [this] {
            m_info->setText("disconnected, retrying ...");
        });
        // 断线自动重连：每秒检查一次
        m_reconn = new QTimer(this);
        connect(m_reconn, &QTimer::timeout, this, [this] {
            if (m_sock->state() != QAbstractSocket::ConnectedState)
                m_sock->connectToHost("127.0.0.1", 9001);
        });
        m_reconn->start(1000);
        m_sock->connectToHost("127.0.0.1", 9001);
    }
private:
    void onReady() {
        m_rbuf.append(m_sock->readAll());          // 字节入桶
        while (true) {                              // ★和网关同款的状态机切帧
            if (m_rbuf.size() < 4) break;
            if ((uint8_t)m_rbuf[0] != kMagic) { m_rbuf.remove(0, 1); continue; }
            uint16_t len = ((uint8_t)m_rbuf[1] << 8) | (uint8_t)m_rbuf[2];
            if (len > 4096) { m_rbuf.remove(0, 1); continue; }
            if (m_rbuf.size() < 4 + (int)len) break;
            uint8_t type = (uint8_t)m_rbuf[3];
            QByteArray payload = m_rbuf.mid(4, len);
            m_rbuf.remove(0, 4 + (int)len);
            if (type == MSG_STAT)
                onStat(QString::fromUtf8(payload));
        }
    }
    void onStat(const QString& s) {                 // "online=..;msgs=..;wakes=..;q=..;r=.."
        long online = 0, msgs = 0, wakes = 0, q = 0, r = 0;
        for (const QString& kv : s.split(';')) {
            QStringList p2 = kv.split('=');
            if (p2.size() != 2) continue;
            if      (p2[0] == "online") online = p2[1].toLong();
            else if (p2[0] == "msgs")   msgs   = p2[1].toLong();
            else if (p2[0] == "wakes")  wakes  = p2[1].toLong();
            else if (p2[0] == "q")      q      = p2[1].toLong();
            else if (p2[0] == "r")      r      = p2[1].toLong();
        }
        double rate = (m_lastMsgs >= 0 && msgs >= m_lastMsgs) ? double(msgs - m_lastMsgs) : 0;
        m_lastMsgs = msgs;                           // 每秒一帧 → 差值即 msg/s
        m_chart->addSample(rate);
        m_info->setText(QString("online=%1   rate=%2 msg/s   msgs=%3   wakes=%4   q=%5   r=%6")
                        .arg(online).arg(rate, 0, 'f', 0).arg(msgs).arg(wakes).arg(q).arg(r));
    }
    QTcpSocket* m_sock;
    QTimer*     m_reconn;
    QLabel*     m_info;
    Chart*      m_chart;
    QByteArray  m_rbuf;
    long        m_lastMsgs = -1;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Panel w;
    w.resize(720, 340);
    w.setWindowTitle("IoT Gateway Monitor");
    w.show();
    return app.exec();
}
