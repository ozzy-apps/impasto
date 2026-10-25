#include "ui/AboutDialog.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("about-dialog"));
    setWindowTitle(tr("About Impasto"));
    setModal(true);
    setMinimumWidth(400);

    // Material 3 dialog: rounded surface, 24 px inset, tonal text button.
    setStyleSheet(QStringLiteral(
        "QDialog#about-dialog { background: #1a1a1c; }"
        "QFrame#about-surface { background: #2d2d33; border-radius: 28px; }"
        "QLabel { background: transparent; color: #f2f2f7; }"
        "QLabel#about-stack, QLabel#about-credit { color: #c4c4cf; }"
        "QPushButton { background: transparent; color: #b5d1ff; border: none;"
        " border-radius: 20px; padding: 10px 24px; min-height: 20px; }"
        "QPushButton:hover, QPushButton:focus { background: #3a465b; }"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 8, 8, 8);
    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("about-surface"));
    outer->addWidget(surface);
    auto* layout = new QVBoxLayout(surface);
    layout->setContentsMargins(24, 24, 24, 16);
    layout->setSpacing(16);
    auto label = [&](const QString& text, const char* name, int size) {
        auto* item = new QLabel(text, surface);
        item->setObjectName(QLatin1String(name));
        item->setFont(theme::font(size));
        item->setAlignment(Qt::AlignCenter);
        item->setWordWrap(true);
        item->setTextInteractionFlags(Qt::TextBrowserInteraction);
        layout->addWidget(item);
        return item;
    };
    auto* logo = label(QString(), "about-logo", 14);
    logo->setPixmap(QIcon(QStringLiteral(":/icons/app.svg")).pixmap(QSize(80, 80), devicePixelRatioF()));
    logo->setAccessibleName(tr("Impasto logo"));
    label(QStringLiteral("Impasto"), "about-title", 24);
    label(tr("Version %1").arg(QApplication::applicationVersion()), "about-version", 16);
    label(tr("Built on C++20 · Qt 6 · OpenGL"), "about-stack", 14);
    label(tr("Made with love by Ozzy <span style=\"color:#ffb4ab\">❤️</span>"), "about-credit", 14);
    auto* support = label(tr("Requests? Bugs?<br><a href=\"mailto:impasto@ozzy.co.nl\" style=\"color:#b5d1ff\">impasto@ozzy.co.nl</a>"), "about-support", 14);
    support->setOpenExternalLinks(true);
    auto* repo = label(QStringLiteral("<a href=\"https://github.com/ozzy-apps/impasto\" style=\"color:#b5d1ff\">github.com/ozzy-apps/impasto</a>"), "about-repository", 14);
    repo->setOpenExternalLinks(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, surface);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("about-close"));
    buttons->button(QDialogButtonBox::Close)->setIcon(QIcon());
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
