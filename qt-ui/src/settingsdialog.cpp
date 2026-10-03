#include "settingsdialog.h"
#include "hostclient.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontComboBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(HostClient *host, const QStringList &profiles, QWidget *parent)
    : QDialog(parent), m_host(host), m_context(host->contextId()) {
    setWindowTitle(tr("Settings"));
    resize(680, 500);
    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);
    auto page = [this](const QString &title) {
        auto *widget = new QWidget(m_tabs);
        auto *form = new QFormLayout(widget);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_tabs->addTab(widget, title);
        return form;
    };
    auto choice = [this](QFormLayout *form, const QString &key, const QString &label,
                         const QStringList &labels, const QStringList &values, bool editable = false) {
        auto *widget = new QComboBox(this);
        widget->setObjectName(key);
        for (int i = 0; i < labels.size(); ++i) widget->addItem(labels[i], values[i]);
        widget->setEditable(editable);
        form->addRow(label, widget); m_fields.insert(key, widget);
    };
    auto text = [this](QFormLayout *form, const QString &key, const QString &label, const QString &hint = QString()) {
        auto *widget = new QLineEdit(this);
        widget->setObjectName(key); widget->setPlaceholderText(hint);
        form->addRow(label, widget); m_fields.insert(key, widget);
    };
    auto check = [this](QFormLayout *form, const QString &key, const QString &label) {
        auto *widget = new QCheckBox(label, this); widget->setObjectName(key);
        form->addRow(widget); m_fields.insert(key, widget);
    };
    auto number = [this](QFormLayout *form, const QString &key, const QString &label, int low, int high) {
        auto *widget = new QSpinBox(this); widget->setObjectName(key); widget->setRange(low, high);
        form->addRow(label, widget); m_fields.insert(key, widget);
    };

    auto *appearance = page(tr("Appearance"));
    choice(appearance, "qt.theme", tr("Interface theme"), {tr("KDE / system"), tr("Light"), tr("Dark")}, {"system", "light", "dark"});
    auto *font = new QFontComboBox(this); font->setObjectName("qt.fontFamily");
    appearance->addRow(tr("Interface font"), font); m_fields.insert("qt.fontFamily", font);
    number(appearance, "qt.fontSize", tr("Interface font size"), 8, 32);
    auto *terminal = page(tr("Terminal"));
    QStringList profileLabels{tr("KDE default profile")}, profileValues{QString()};
    for (const auto &profile : profiles) if (!profileValues.contains(profile)) {
        profileLabels.append(profile); profileValues.append(profile);
    }
    choice(terminal, "qt.konsoleProfile", tr("Konsole profile"), profileLabels, profileValues);
    auto *note = new QLabel(tr("New terminals use this Konsole profile, including its shell, colors and font.\nEdit profiles using Konsole’s Settings → Manage Profiles."), this);
    note->setWordWrap(true); terminal->addRow(note);

    auto *agents = page(tr("Agents"));
    choice(agents, "defaultAgent", tr("Default agent"), {tr("Claude"), tr("Codex")}, {"claude-code", "codex-agent"});
    text(agents, "defaultClaudeModel", tr("Claude model"), tr("Agent default"));
    text(agents, "defaultCodexModel", tr("Codex model"), tr("Agent default"));
    choice(agents, "defaultEffort", tr("Claude effort"), {tr("Agent default"), "low", "medium", "high", "max"}, {"", "low", "medium", "high", "max"});
    choice(agents, "defaultCodexEffort", tr("Codex effort"), {tr("Agent default"), "low", "medium", "high", "xhigh"}, {"", "low", "medium", "high", "xhigh"});
    auto *agentNote = new QLabel(tr("Defaults apply to new agent tabs. Existing sessions keep their configuration."), this);
    agentNote->setWordWrap(true); agents->addRow(agentNote);
    auto *workspaces = page(tr("Workspaces"));
    number(workspaces, "defaultTerminalCount", tr("Konsole tabs in a new workspace"), 1, 10);
    check(workspaces, "createDefaultAgentTerminal", tr("Also create a default agent tab"));
    check(workspaces, "closeTerminalAfterProcessExit", tr("Close terminal tabs when their process exits"));
    auto *python = page(tr("Python"));
    check(python, "agentPythonVenvEnabled", tr("Use a Python virtual environment for agents"));
    text(python, "agentPythonVenvPath", tr("Virtual environment path"), ".venv");
    auto *pythonNote = new QLabel(tr("A relative path is resolved from the agent’s workspace on the host. Applies when a new session starts."), this);
    pythonNote->setWordWrap(true); python->addRow(pythonNote);
    m_status = new QLabel(tr("Loading settings…"), this); m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_save = buttons->button(QDialogButtonBox::Save);
    m_save->setObjectName("saveSettings");
    m_retry = buttons->addButton(tr("Reload"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_retry, &QPushButton::clicked, this, &SettingsDialog::load);
    setSettings({});
    load();
}

bool SettingsDialog::decodeSettings(const QJsonValue &value, QJsonObject &settings, QString &error) {
    error.clear();
    if (value.isNull() || value.isUndefined()) { settings = {}; return true; }
    if (value.isObject()) { settings = value.toObject(); return true; }
    if (value.isString()) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(value.toString().toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            settings = document.object(); return true;
        }
    }
    error = tr("The host returned invalid settings. Saving is disabled to protect the existing configuration.");
    return false;
}

void SettingsDialog::setSettings(const QJsonObject &settings) {
    m_original = settings;
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        const bool qt = it.key().startsWith("qt.");
        const QString key = qt ? it.key().mid(3) : it.key();
        const QJsonValue value = qt ? settings["qtUi"].toObject()[key] : settings[key];
        auto *widget = it.value();
        if (auto *font = qobject_cast<QFontComboBox *>(widget)) {
            font->setCurrentFont(QFont(value.toString(QFont().family())));
        } else if (auto *choice = qobject_cast<QComboBox *>(widget)) {
            QString fallback;
            if (key == "theme") fallback = "system";
            if (key == "defaultAgent") fallback = "claude-code";
            const QString selected = value.toString(fallback);
            int index = choice->findData(selected);
            if (index < 0) { choice->addItem(selected, selected); index = choice->count() - 1; }
            choice->setCurrentIndex(index);
        } else if (auto *text = qobject_cast<QLineEdit *>(widget)) {
            text->setText(value.toString(key == "agentPythonVenvPath" ? ".venv" : QString()));
        } else if (auto *number = qobject_cast<QSpinBox *>(widget)) {
            number->setValue(value.toInt(key == "fontSize" ? (QFont().pointSize() > 0 ? QFont().pointSize() : 10) : 1));
        } else if (auto *check = qobject_cast<QCheckBox *>(widget)) check->setChecked(value.toBool());
    }
}

QJsonObject SettingsDialog::settings() const {
    auto result = m_original;
    auto qt = result["qtUi"].toObject();
    for (auto it = m_fields.cbegin(); it != m_fields.cend(); ++it) {
        QJsonValue value;
        auto *widget = it.value();
        if (auto *font = qobject_cast<QFontComboBox *>(widget)) value = font->currentFont().family();
        else if (auto *choice = qobject_cast<QComboBox *>(widget)) value = choice->currentData().toString();
        else if (auto *text = qobject_cast<QLineEdit *>(widget)) value = text->text().trimmed();
        else if (auto *number = qobject_cast<QSpinBox *>(widget)) value = number->value();
        else if (auto *check = qobject_cast<QCheckBox *>(widget)) value = check->isChecked();
        if (it.key().startsWith("qt.")) qt.insert(it.key().mid(3), value);
        else result.insert(it.key(), value);
    }
    result.insert("qtUi", qt);
    return result;
}

void SettingsDialog::load() {
    m_tabs->setEnabled(false); m_save->setEnabled(false);
    if (!m_host->ready() || m_context.isEmpty() || m_context != m_host->contextId()) {
        m_status->setText(tr("Connect to an agent host and open a profile, then reopen Settings.")); return;
    }
    m_retry->setEnabled(false); m_status->setText(tr("Loading settings…"));
    m_host->invoke("settings:load", {}, this, [this](const QJsonValue &value, const QString &error) {
        m_retry->setEnabled(true);
        if (m_context != m_host->contextId()) { m_status->setText(tr("The profile changed. Reopen Settings.")); return; }
        QJsonObject settings; QString parseError;
        if (!error.isEmpty() || !decodeSettings(value, settings, parseError)) {
            m_status->setText(error.isEmpty() ? parseError : error); return;
        }
        setSettings(settings); m_tabs->setEnabled(true); m_save->setEnabled(true);
        m_status->setText(tr("Changes are saved to the active host. Other settings and the built-in status line configuration are preserved."));
    });
}

void SettingsDialog::save() {
    if (!m_host->ready() || m_context != m_host->contextId()) {
        m_status->setText(tr("The host or profile changed. Reopen Settings.")); return;
    }
    const auto proposed = settings();
    if (proposed["agentPythonVenvEnabled"].toBool() && proposed["agentPythonVenvPath"].toString().isEmpty()) {
        m_status->setText(tr("Enter a virtual environment path, such as .venv.")); return;
    }
    m_tabs->setEnabled(false); m_save->setEnabled(false); m_retry->setEnabled(false);
    m_status->setText(tr("Saving…"));
    m_host->invoke("settings:save", {{"data", QString::fromUtf8(QJsonDocument(proposed).toJson(QJsonDocument::Compact))}}, this,
        [this, proposed](const QJsonValue &value, const QString &error) {
            if (m_context != m_host->contextId()) { m_status->setText(tr("The profile changed. Reopen Settings.")); return; }
            if (!error.isEmpty() || value == false) {
                m_tabs->setEnabled(true); m_save->setEnabled(true); m_retry->setEnabled(true);
                m_status->setText(error.isEmpty() ? tr("The host did not save the settings.") : error); return;
            }
            emit settingsSaved(proposed); accept();
        });
}
