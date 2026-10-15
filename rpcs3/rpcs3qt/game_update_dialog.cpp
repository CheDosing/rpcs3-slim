#include "stdafx.h"
#include "game_update_dialog.h"
#include "main_window.h"

#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Loader/PSF.h"
#include "Utilities/File.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

LOG_CHANNEL(game_update_log, "GameUpdate");

namespace
{
	enum column : int
	{
		col_name = 0,
		col_size,
		col_firmware,
		col_status,
		col_count
	};

	const QString all_games_key = QStringLiteral("*");

	double version_number(const QString& ver)
	{
		return std::strtod(ver.toStdString().c_str(), nullptr);
	}

	QString format_size(quint64 size)
	{
		if (size >= 1024ull * 1024 * 1024)
			return QString::number(static_cast<double>(size) / (1024.0 * 1024 * 1024), 'f', 2) + QStringLiteral(" GB");
		if (size >= 1024ull * 1024)
			return QString::number(static_cast<double>(size) / (1024.0 * 1024), 'f', 1) + QStringLiteral(" MB");
		return QString::number(static_cast<double>(size) / 1024.0, 'f', 0) + QStringLiteral(" KB");
	}

	QString format_fw(const QString& fw)
	{
		// "03.4000" -> "3.40"
		const double v = version_number(fw);
		return v > 0 ? QString::number(v, 'f', 2) : (fw.isEmpty() ? QStringLiteral("-") : fw);
	}
}

QString game_update_dialog::find_rusty_psn()
{
	const QString exe_name =
#ifdef _WIN32
		QStringLiteral("rusty-psn-cli.exe");
#else
		QStringLiteral("rusty-psn-cli");
#endif

	const QString app_dir = QCoreApplication::applicationDirPath();

	for (const QString& dir : { app_dir, app_dir + QStringLiteral("/../lib/rpcs3-slim") })
	{
		if (const QFileInfo info(dir + QStringLiteral("/") + exe_name); info.isFile() && info.isExecutable())
		{
			return info.canonicalFilePath();
		}
	}

	return QStandardPaths::findExecutable(exe_name);
}

game_update_dialog::game_update_dialog(main_window* mw, const QString& rusty_psn_path, std::vector<title_entry> titles, const std::string& preselect_serial, QWidget* parent)
	: QDialog(parent)
	, m_mw(mw)
	, m_rusty_psn(rusty_psn_path)
	, m_titles(std::move(titles))
{
	setWindowTitle(tr("Game Update Downloader"));
	setObjectName("game_update_dialog");
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(520, 360);
	resize(720, 480);

	// Title selection: a game from the list, every game, or free text (one or more Title IDs)
	m_title_box = new QComboBox(this);
	m_title_box->setEditable(true);
	m_title_box->setInsertPolicy(QComboBox::NoInsert);
	m_title_box->lineEdit()->setPlaceholderText(tr("Title ID(s), e.g. BLUS30443"));

	std::sort(m_titles.begin(), m_titles.end(), [](const title_entry& a, const title_entry& b) { return a.name < b.name; });

	if (!m_titles.empty())
	{
		m_title_box->addItem(tr("All games in your list (%0)").arg(m_titles.size()), all_games_key);
	}

	int preselect_index = -1;
	for (const title_entry& t : m_titles)
	{
		if (t.serial == preselect_serial)
			preselect_index = m_title_box->count();

		m_title_box->addItem(QString::fromStdString(t.name + "  (" + t.serial + ")"), QString::fromStdString(t.serial));
	}

	m_title_box->setCurrentIndex(preselect_index);
	if (preselect_index < 0)
		m_title_box->setEditText(QString::fromStdString(preselect_serial));

	m_check_button = new QPushButton(tr("Check for Updates"), this);
	m_check_button->setAutoDefault(false);

	QHBoxLayout* top = new QHBoxLayout();
	top->addWidget(m_title_box, 1);
	top->addWidget(m_check_button);

	m_header_label = new QLabel(tr("Pick a game, all games, or type Title IDs, then press \"Check for Updates\". Updates come from Sony's PS3 update server and are verified before they are installed."), this);
	m_header_label->setWordWrap(true);

	// Update list: one branch per game, one checkable row per update
	m_tree = new QTreeWidget(this);
	m_tree->setColumnCount(col_count);
	m_tree->setHeaderLabels({ tr("Update"), tr("Size"), tr("Requires FW"), tr("Status") });
	m_tree->header()->setStretchLastSection(true);
	m_tree->header()->setSectionResizeMode(col_name, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(col_size, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(col_firmware, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(col_status, QHeaderView::Interactive);
	m_tree->header()->resizeSection(col_status, 180);
	m_tree->setSelectionMode(QAbstractItemView::NoSelection);
	m_tree->setFocusPolicy(Qt::NoFocus);
	m_tree->setAlternatingRowColors(true);
	m_tree->setRootIsDecorated(true);
	m_tree->setUniformRowHeights(true);

	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 1000);
	m_progress->setTextVisible(false);
	m_progress->setVisible(false);

	m_status_label = new QLabel(this);
	m_status_label->setWordWrap(true);

	m_install_check = new QCheckBox(tr("Install updates when the download finishes"), this);
	m_install_check->setChecked(true);

	QPushButton* folder_button = new QPushButton(tr("Open Download Folder"), this);
	folder_button->setAutoDefault(false);

	m_download_button = new QPushButton(tr("Download && Install"), this);
	m_download_button->setDefault(true);

	m_close_button = new QPushButton(tr("Close"), this);
	m_close_button->setAutoDefault(false);

	QHBoxLayout* bottom = new QHBoxLayout();
	bottom->addWidget(folder_button);
	bottom->addStretch(1);
	bottom->addWidget(m_close_button);
	bottom->addWidget(m_download_button);

	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(m_header_label);
	layout->addWidget(m_tree, 1);
	layout->addWidget(m_progress);
	layout->addWidget(m_status_label);
	layout->addWidget(m_install_check);
	layout->addLayout(bottom);
	setLayout(layout);

	connect(m_check_button, &QPushButton::clicked, this, [this]() { check_updates(); });
	connect(m_title_box->lineEdit(), &QLineEdit::returnPressed, this, [this]() { check_updates(); });
	connect(m_title_box, &QComboBox::activated, this, [this]() { check_updates(); });
	connect(m_download_button, &QPushButton::clicked, this, [this]() { download_selected(); });
	connect(m_install_check, &QCheckBox::toggled, this, [this](bool install)
	{
		m_download_button->setText(install ? tr("Download && Install") : tr("Download"));
	});
	connect(m_close_button, &QPushButton::clicked, this, [this]()
	{
		if (m_phase != phase::idle && m_process)
		{
			// Stop the running check/download but keep the dialog open
			m_cancelled = true;
			set_status(tr("Cancelling..."));
			m_process->kill();
			return;
		}

		QDialog::reject();
	});
	connect(folder_button, &QPushButton::clicked, this, [this]()
	{
		QDesktopServices::openUrl(QUrl::fromLocalFile(download_dir()));
	});

	update_controls();

	if (!preselect_serial.empty())
	{
		QTimer::singleShot(0, this, [this]() { check_updates(); });
	}
}

game_update_dialog::~game_update_dialog()
{
	if (m_process && m_process->state() != QProcess::NotRunning)
	{
		m_process->disconnect(this);
		m_process->kill();
		m_process->waitForFinished(2000);
	}
}

void game_update_dialog::reject()
{
	if (m_phase != phase::idle && m_process)
	{
		// Window closed while busy: stop rusty-psn first, the dialog closes once it has exited
		m_cancelled = true;
		m_close_requested = true;
		set_status(tr("Cancelling..."));
		m_process->kill();
		return;
	}

	QDialog::reject();
}

QString game_update_dialog::download_dir() const
{
	const std::string dir = fs::get_cache_dir() + "game_updates";
	fs::create_path(dir);
	return QDir::toNativeSeparators(QDir(QString::fromStdString(dir)).absolutePath());
}

QStringList game_update_dialog::requested_serials() const
{
	QStringList serials;
	const int index = m_title_box->currentIndex();

	if (index >= 0 && m_title_box->currentText() == m_title_box->itemText(index))
	{
		const QString key = m_title_box->itemData(index).toString();

		if (key == all_games_key)
		{
			for (const title_entry& t : m_titles)
				serials << QString::fromStdString(t.serial);
		}
		else
		{
			serials << key;
		}

		return serials;
	}

	// Free text: every Title ID in it, dashes and case ignored (BLUS-30443, blus30443)
	static const QRegularExpression serial_expr(QStringLiteral("\\b([A-Z]{4})-?([0-9]{5})\\b"));
	const QString text = m_title_box->currentText().toUpper();

	for (QRegularExpressionMatchIterator it = serial_expr.globalMatch(text); it.hasNext();)
	{
		const QRegularExpressionMatch match = it.next();
		const QString serial = match.captured(1) + match.captured(2);

		if (!serials.contains(serial))
			serials << serial;
	}

	return serials;
}

std::string game_update_dialog::installed_version(const std::string& serial) const
{
	// Installed updates (and HDD games) live in dev_hdd0/game/<serial>/
	const std::string sfo_path = rpcs3::utils::get_hdd0_dir() + "game/" + serial + "/PARAM.SFO";

	if (fs::is_file(sfo_path))
	{
		const psf::registry sfo = psf::load_object(sfo_path);

		if (const std::string ver(psf::get_string(sfo, "APP_VER")); !ver.empty())
			return ver;
	}

	for (const title_entry& t : m_titles)
	{
		if (t.serial == serial)
			return t.app_ver;
	}

	return {};
}

void game_update_dialog::set_status(const QString& text)
{
	m_status_label->setText(text);
}

void game_update_dialog::update_controls()
{
	const bool busy = m_phase != phase::idle;
	const bool any_rows = !m_rows.empty();

	m_title_box->setEnabled(!busy);
	m_check_button->setEnabled(!busy);
	m_install_check->setEnabled(!busy);
	m_download_button->setEnabled(!busy && any_rows);
	m_close_button->setText(busy ? tr("Cancel") : tr("Close"));
	m_progress->setVisible(busy);

	if (m_phase == phase::checking)
		m_progress->setRange(0, 0); // busy indicator
	else
		m_progress->setRange(0, 1000);

	const auto set_checkable = [busy](QTreeWidgetItem* item)
	{
		Qt::ItemFlags flags = item->flags();
		flags.setFlag(Qt::ItemIsUserCheckable, !busy);
		item->setFlags(flags);
	};

	for (package_row& row : m_rows)
		set_checkable(row.item);

	for (const auto& [title_id, item] : m_title_items)
		set_checkable(item);
}

void game_update_dialog::start_process(const QStringList& args, phase new_phase)
{
	if (m_process)
	{
		m_process->deleteLater();
	}

	m_process = new QProcess(this);
	m_process->setProgram(m_rusty_psn);
	m_process->setArguments(args);
	m_process->setWorkingDirectory(download_dir()); // rusty-psn writes its log file to the working directory

#ifdef _WIN32
	// rusty-psn-cli is a console program; keep it from opening a console window
	m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* cpa)
	{
		cpa->flags |= CREATE_NO_WINDOW;
	});
#endif

	m_output.clear();
	m_cancelled = false;
	m_phase = new_phase;

	connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() { on_output(); });
	connect(m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { on_process_finished(); });
	connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
	{
		if (error != QProcess::FailedToStart)
			return;

		game_update_log.error("Could not start %s: %s", m_rusty_psn.toStdString(), m_process->errorString().toStdString());
		m_phase = phase::idle;
		set_status(tr("Could not start rusty-psn: %0").arg(m_process->errorString()));
		update_controls();
	});

	game_update_log.notice("Running %s %s", m_rusty_psn.toStdString(), args.join(' ').toStdString());
	m_process->start();
	update_controls();
}

void game_update_dialog::check_updates()
{
	if (m_phase != phase::idle)
		return;

	const QStringList serials = requested_serials();

	if (serials.isEmpty())
	{
		set_status(tr("\"%0\" doesn't contain a valid PS3 Title ID (4 letters + 5 digits, e.g. BLUS30443).").arg(m_title_box->currentText()));
		return;
	}

	m_tree->clear();
	m_rows.clear();
	m_title_items.clear();
	m_errors.clear();

	set_status(serials.size() == 1
		? tr("Checking for updates for %0...").arg(serials.front())
		: tr("Checking for updates for %0 games...").arg(serials.size()));

	start_process({ QStringLiteral("--json"), QStringLiteral("--list"), QStringLiteral("-t"), serials.join(' ') }, phase::checking);
}

void game_update_dialog::download_selected()
{
	if (m_phase != phase::idle)
		return;

	std::map<QString, QStringList> selection;
	QStringList serials;

	m_run_total = 0;
	m_run_done = 0;
	m_current_bytes = 0;
	m_run_count = 0;
	m_run_index = 0;
	m_run_failed = 0;
	m_finished_paths.clear();
	m_errors.clear();

	for (package_row& row : m_rows)
	{
		if (row.item->checkState(col_name) != Qt::Checked)
			continue;

		if (!selection.contains(row.title_id))
			serials << row.title_id;

		selection[row.title_id] << QString::number(row.index);
		m_run_total += row.size;
		m_run_count++;
		set_row_status(row, tr("Queued"));
	}

	if (!m_run_count)
	{
		set_status(tr("Tick at least one update to download."));
		return;
	}

	QStringList args = { QStringLiteral("--json"), QStringLiteral("-t"), serials.join(' '), QStringLiteral("-d"), download_dir() };

	for (const auto& [serial, indices] : selection)
	{
		args << QStringLiteral("--select") << serial + QStringLiteral(":") + indices.join(',');
	}

	m_progress->setValue(0);
	set_status(tr("Starting download of %0 update(s) (%1)...").arg(m_run_count).arg(format_size(m_run_total)));
	start_process(args, phase::downloading);
}

void game_update_dialog::on_output()
{
	m_output += m_process->readAllStandardOutput();

	qsizetype newline = 0;
	while ((newline = m_output.indexOf('\n')) >= 0)
	{
		const QByteArray line = m_output.left(newline).trimmed();
		m_output.remove(0, newline + 1);

		if (line.isEmpty())
			continue;

		QJsonParseError error{};
		const QJsonDocument doc = QJsonDocument::fromJson(line, &error);

		if (error.error != QJsonParseError::NoError || !doc.isObject())
		{
			game_update_log.warning("rusty-psn: %s", line.toStdString());
			continue;
		}

		on_event(doc.object());
	}
}

void game_update_dialog::on_event(const QJsonObject& obj)
{
	const QString event = obj.value("event").toString();
	const QString title_id = obj.value("title_id").toString();
	const int index = obj.value("index").toInt(-1);

	if (m_phase == phase::downloading && (event == "title" || event == "error"))
	{
		// The download run queries the server again; the list is already shown
		if (event == "error")
			m_errors << title_id + QStringLiteral(": ") + obj.value("message").toString();
		return;
	}

	if (event == "title")
	{
		add_title(obj);
	}
	else if (event == "error")
	{
		// Show the game anyway so every requested title gets an answer
		QString name = title_id;
		for (const title_entry& t : m_titles)
			if (QString::fromStdString(t.serial) == title_id)
				name = QString::fromStdString(t.name + "  (" + t.serial + ")");

		QTreeWidgetItem* item = new QTreeWidgetItem(m_tree);
		item->setText(col_name, name);
		item->setText(col_status, obj.value("message").toString());
		item->setToolTip(col_status, obj.value("message").toString());
		item->setFlags(Qt::ItemIsEnabled);
		item->setForeground(col_status, palette().placeholderText());
		m_errors << title_id + QStringLiteral(": ") + obj.value("message").toString();
	}
	else if (package_row* row = find_row(title_id, index))
	{
		if (event == "start")
		{
			m_run_index++;
			m_current_bytes = 0;
			set_row_status(*row, tr("Starting..."));
			m_tree->scrollToItem(row->item);
		}
		else if (event == "progress")
		{
			const quint64 downloaded = static_cast<quint64>(obj.value("downloaded").toDouble());
			const quint64 total = std::max<quint64>(static_cast<quint64>(obj.value("total").toDouble()), 1);

			if (!row->bar)
			{
				row->item->setText(col_status, {});
				row->bar = new QProgressBar(m_tree);
				row->bar->setRange(0, 1000);
				row->bar->setTextVisible(true);
				row->bar->setFormat(QStringLiteral("%p%"));
				m_tree->setItemWidget(row->item, col_status, row->bar);
			}

			row->bar->setValue(static_cast<int>(std::min<quint64>(downloaded, total) * 1000 / total));
			m_current_bytes = downloaded;
			update_overall_progress();
		}
		else if (event == "verifying")
		{
			// Sent once before the download (checking an existing file) and once after it
			set_row_status(*row, m_current_bytes ? tr("Verifying...") : tr("Checking existing file..."));
		}
		else if (event == "finished")
		{
			m_run_done += row->size;
			m_current_bytes = 0;
			m_finished_paths << obj.value("path").toString();
			set_row_status(*row, tr("Downloaded"));
			update_overall_progress();
		}
		else if (event == "failed")
		{
			const QString message = obj.value("message").toString();
			m_run_done += row->size;
			m_current_bytes = 0;
			m_run_failed++;
			m_errors << row->title_id + QStringLiteral(" ") + row->version + QStringLiteral(": ") + message;
			set_row_status(*row, tr("Failed"));
			row->item->setToolTip(col_status, message);
			game_update_log.error("Download of %s %s failed: %s", row->title_id.toStdString(), row->version.toStdString(), message.toStdString());
			update_overall_progress();
		}
	}
}

void game_update_dialog::add_title(const QJsonObject& obj)
{
	const QString title_id = obj.value("title_id").toString();
	const QString title = obj.value("title").toString();
	const QString installed = QString::fromStdString(installed_version(title_id.toStdString()));
	const double installed_num = installed.isEmpty() ? 0.0 : version_number(installed);

	QTreeWidgetItem* parent = new QTreeWidgetItem(m_tree);
	parent->setText(col_name, (title.isEmpty() ? title_id : title + QStringLiteral("  (") + title_id + QStringLiteral(")")));
	parent->setText(col_status, installed.isEmpty() ? tr("Not installed") : tr("Installed: %0").arg(installed));
	parent->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);

	QFont bold = parent->font(col_name);
	bold.setBold(true);
	parent->setFont(col_name, bold);

	quint64 title_size = 0;
	int pending = 0;

	for (const QJsonValue& value : obj.value("packages").toArray())
	{
		const QJsonObject pkg = value.toObject();
		const QString version = pkg.value("version").toString();
		const quint64 size = static_cast<quint64>(pkg.value("size").toDouble());
		const bool already = installed_num > 0.0 && version_number(version) <= installed_num;

		QTreeWidgetItem* item = new QTreeWidgetItem(parent);
		item->setText(col_name, tr("Version %0").arg(pkg.value("id").toString()));
		item->setToolTip(col_name, pkg.value("file_name").toString());
		item->setText(col_size, format_size(size));
		item->setText(col_firmware, format_fw(pkg.value("system_ver").toString()));
		item->setText(col_status, already ? tr("Installed") : tr("Available"));
		item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		item->setCheckState(col_name, already ? Qt::Unchecked : Qt::Checked);

		m_rows.push_back({ title_id, pkg.value("index").toInt(), size, version, item, nullptr });
		title_size += size;
		pending += already ? 0 : 1;
	}

	parent->setText(col_size, format_size(title_size));
	parent->setExpanded(true);

	if (pending == 0)
		parent->setCheckState(col_name, Qt::Unchecked);

	m_title_items[title_id] = parent;
	update_controls();
}

game_update_dialog::package_row* game_update_dialog::find_row(const QString& title_id, int index)
{
	for (package_row& row : m_rows)
	{
		if (row.title_id == title_id && row.index == index)
			return &row;
	}

	return nullptr;
}

void game_update_dialog::set_row_status(package_row& row, const QString& text)
{
	if (row.bar)
	{
		m_tree->removeItemWidget(row.item, col_status);
		row.bar = nullptr;
	}

	row.item->setText(col_status, text);
}

void game_update_dialog::update_overall_progress()
{
	const quint64 done = m_run_done + m_current_bytes;
	m_progress->setValue(m_run_total ? static_cast<int>(std::min(done, m_run_total) * 1000 / m_run_total) : 0);
	set_status(tr("Downloading update %0 of %1  -  %2 / %3")
		.arg(std::max(m_run_index, 1)).arg(m_run_count).arg(format_size(done)).arg(format_size(m_run_total)));
}

void game_update_dialog::on_process_finished()
{
	on_output(); // flush anything left in the pipe

	const phase finished_phase = m_phase;
	const QString stderr_text = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
	const bool crashed = !m_cancelled && (m_process->exitStatus() != QProcess::NormalExit || m_process->exitCode() != 0);

	if (crashed)
	{
		game_update_log.error("rusty-psn exited with code %d: %s", m_process->exitCode(), stderr_text.toStdString());
	}

	m_phase = phase::idle;
	update_controls();

	if (m_close_requested)
	{
		QDialog::reject();
		return;
	}

	if (finished_phase == phase::checking)
	{
		int available = 0;
		for (const package_row& row : m_rows)
			available += row.item->checkState(col_name) == Qt::Checked ? 1 : 0;

		// The controls were disabled during the check, so focus ended up elsewhere; Enter should now download
		if (!m_rows.empty())
			m_download_button->setFocus();

		if (m_cancelled)
			set_status(tr("Update check cancelled."));
		else if (crashed)
			set_status(tr("rusty-psn stopped unexpectedly.\n%0").arg(stderr_text));
		else if (m_rows.empty())
			set_status(m_errors.size() == 1 ? m_errors.front() : tr("No updates were found."));
		else if (available)
			set_status(tr("%0 update(s) ready to download. Updates install in order, oldest first.").arg(available));
		else
			set_status(tr("Everything is up to date. You can still tick a version to download it again."));

		return;
	}

	// Download run
	for (package_row& row : m_rows)
	{
		if (row.item->text(col_status) == tr("Queued") || row.item->text(col_status) == tr("Starting...") || row.bar)
			set_row_status(row, m_cancelled ? tr("Cancelled") : tr("Failed"));
	}

	if (m_cancelled)
	{
		m_progress->setValue(0);
		set_status(tr("Download cancelled. %0 update(s) finished before that and are kept in the download folder.").arg(m_finished_paths.size()));
		return;
	}

	if (crashed && m_finished_paths.isEmpty())
	{
		set_status(tr("rusty-psn stopped unexpectedly.\n%0").arg(stderr_text));
		return;
	}

	install_finished();
}

void game_update_dialog::install_finished()
{
	if (m_finished_paths.isEmpty())
	{
		set_status(tr("No updates were downloaded.\n%0").arg(m_errors.join('\n')));
		return;
	}

	QStringList paths = m_finished_paths;

	// File names carry the app version (e.g. ...-A0102-V0100-PE.pkg), so name order is install order
	std::sort(paths.begin(), paths.end(), [](const QString& a, const QString& b)
	{
		return QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0;
	});

	const QString failed = m_run_failed ? tr("\n%0 update(s) failed:\n%1").arg(m_run_failed).arg(m_errors.join('\n')) : QString();

	if (!m_install_check->isChecked())
	{
		set_status(tr("%0 update(s) downloaded to %1.").arg(paths.size()).arg(download_dir()) + failed);
		return;
	}

	if (!Emu.IsStopped())
	{
		set_status(tr("%0 update(s) downloaded. Stop the running game, then install them with File > Install Packages (download folder: %1).").arg(paths.size()).arg(download_dir()) + failed);
		return;
	}

	if (m_run_failed)
	{
		// Keep the dialog open so the failures stay visible; install what did arrive
		set_status(tr("Installing %0 downloaded update(s).").arg(paths.size()) + failed);
		main_window::InstallPackages(m_mw, paths);
		return;
	}

	game_update_log.notice("Installing %d downloaded update(s)", paths.size());

	// Close first so the installer's progress dialog isn't stacked under this one
	main_window* mw = m_mw;
	accept();
	QTimer::singleShot(0, mw, [mw, paths]() { main_window::InstallPackages(mw, paths); });
}
