#include "Glyphs.h"
#include <QGuiApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextBoundaryFinder>
#include <QDebug>
#include <cmath>

/** 直接验证实际绘制、度量和换行，不以业务状态存在代替字符可见。 */
int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    const auto root = application.arguments().value(1);
    Glyphs glyphs(root);
    bool passed = true;
    const auto check = [&](bool valid, const char *name) {
        qInfo() << "Glyph check:" << name << valid;
        passed &= valid;
    };
    QFile source(root + "/misans-glyphs.json");
    check(source.open(QIODevice::ReadOnly), "load baseline glyph metrics");
    const auto regular =
        QJsonDocument::fromJson(source.readAll()).object()["regular"].toObject()["glyphs"].toObject();
    const QString baseline = QStringLiteral("RHINE MUSIC / 2026");
    double expected = 0;
    for (QChar character : baseline) {
        expected += regular[QString(character)].toObject()["advance"].toDouble() * 24;
    }
    check(std::abs(glyphs.width(baseline, 24) - expected) < 0.000001, "existing Latin metrics remain exact");

    const QString korean = QStringLiteral("밤하늘의 별빛을 따라");
    const QStringList samples{QStringLiteral("中文与 English"),
                              QStringLiteral("君の声が聞こえる 髙﨑"),
                              korean,
                              korean.normalized(QString::NormalizationForm_D),
                              QStringLiteral("Cafe\u0301 か\u3099 𠮷野家"),
                              QStringLiteral("مرحبا بالعالم")};
    QImage preview(1100, 440, QImage::Format_ARGB32_Premultiplied);
    preview.fill(Qt::white);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    for (int row = 0; row < samples.size(); ++row) {
        const auto &text = samples[row];
        const auto bounds = glyphs.bounds(text, 32);
        check(bounds["width"].toDouble() > 0 && bounds["height"].toDouble() > 0,
              "multilingual text has visible outlines");
        const double advance = glyphs.width(text, 32);
        check(std::isfinite(advance) && advance > 0 && advance < 1000,
              "multilingual advance is finite and bounded");
        glyphs.text(painter, text, 30, 20 + row * 65, 32, "regular", 0, Qt::black);
    }
    painter.end();
    int koreanInk = 0;
    for (int y = 150; y < 210; ++y) {
        for (int x = 30; x < 800; ++x) {
            koreanInk += preview.pixelColor(x, y).lightness() < 128;
        }
    }
    check(koreanInk > 100, "Korean lyrics paint visible pixels");
    check(std::abs(glyphs.width(korean, 32) -
                   glyphs.width(korean.normalized(QString::NormalizationForm_D), 32)) < 0.001,
          "composed and decomposed Hangul use consistent metrics");
    const QString clusters = QStringLiteral("e\u0301か\u3099𠮷가");
    const auto lines = glyphs.wrap(clusters, 25, 32);
    check(lines.join(QString()) == clusters, "wrapping preserves all Unicode content");
    for (const auto &line : lines) {
        check(!line.isEmpty() && !line.front().isLowSurrogate() && !line.back().isHighSurrogate() &&
                  !line.front().isMark(),
              "wrapping never splits surrogate pairs or combining sequences");
    }
    const auto previewPath = application.arguments().value(2);
    if (!previewPath.isEmpty()) {
        QDir().mkpath(QFileInfo(previewPath).absolutePath());
        check(preview.save(previewPath), "save multilingual rendering preview");
    }
    return passed ? 0 : 1;
}
