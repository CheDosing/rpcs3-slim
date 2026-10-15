#include "stdafx.h"
#include "game_update_dialog.h"
#include "main_window.h"
#include "curl_handle.h"

#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Loader/PSF.h"
#include "Utilities/File.h"
#include "util/logs.hpp"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QXmlStreamReader>

#include <algorithm>
#include <cctype>
#include <functional>
#include <chrono>
#include <cstdlib>

LOG_CHANNEL(game_update_log, "GameUpdate");

namespace
{
	enum column : int
	{
		col_version = 0,
		col_size,
		col_firmware,
		col_status,
		col_count
	};

	bool is_valid_serial(const std::string& serial)
	{
		// e.g. BLUS30443, NPEB00874, BCES01584
		if (serial.size() != 9)
			return false;

		for (usz i = 0; i < 4; i++)
			if (!std::isalpha(static_cast<unsigned char>(serial[i])))
				return false;

		for (usz i = 4; i < 9; i++)
			if (!std::isdigit(static_cast<unsigned char>(serial[i])))
				return false;

		return true;
	}

	double version_number(const std::string& ver)
	{
		return std::strtod(ver.c_str(), nullptr);
	}

	QString format_size(u64 size)
	{
		if (size >= 1024ull * 1024 * 1024)
			return QString::number(static_cast<double>(size) / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
		if (size >= 1024ull * 1024)
			return QString::number(static_cast<double>(size) / (1024.0 * 1024), 'f', 1) + " MB";
		return QString::number(static_cast<double>(size) / 1024.0, 'f', 0) + " KB";
	}

	QString format_fw(const std::string& fw)
	{
		// "03.4000" -> "3.40"
		if (fw.empty())
			return QStringLiteral("-");

		const double v = std::strtod(fw.c_str(), nullptr);
		return v > 0 ? QString::number(v, 'f', 2) : QString::fromStdString(fw);
	}

	void apply_common_curl_options(rpcs3::curl::curl_handle& curl)
	{
		curl_easy_setopt(curl.get_curl(), CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl.get_curl(), CURLOPT_CONNECTTIMEOUT, 20L);
		// Abort if the transfer stalls below 1 KB/s for 60 seconds
		curl_easy_setopt(curl.get_curl(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
		curl_easy_setopt(curl.get_curl(), CURLOPT_LOW_SPEED_TIME, 60L);
		curl_easy_setopt(curl.get_curl(), CURLOPT_FAILONERROR, 1L);
	}

	size_t write_string_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
		return size * nmemb;
	}

	// The server's sha1sum is checked against the whole file and, to be safe, also against
	// the file without its 32-byte PKG footer (which itself carries a SHA-1 of the package body).
	struct pkg_hasher
	{
		QCryptographicHash full{QCryptographicHash::Sha1};
		QCryptographicHash body{QCryptographicHash::Sha1};
		std::string tail;

		void add(const char* data, usz len)
		{
			full.addData(QByteArrayView(data, static_cast<qsizetype>(len)));
			tail.append(data, len);

			if (tail.size() > 32)
			{
				const usz n = tail.size() - 32;
				body.addData(QByteArrayView(tail.data(), static_cast<qsizetype>(n)));
				tail.erase(0, n);
			}
		}

		std::string full_hex() const
		{
			return full.result().toHex().toStdString();
		}

		bool matches(const std::string& expected) const
		{
			return full_hex() == expected || body.result().toHex().toStdString() == expected;
		}
	};

	struct file_download_ctx
	{
		fs::file file;
		pkg_hasher hash;
	};

	size_t write_file_cb(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		auto* ctx = static_cast<file_download_ctx*>(userdata);
		const usz len = size * nmemb;
		ctx->hash.add(ptr, len);
		return static_cast<size_t>(ctx->file.write(ptr, len));
	}

	struct progress_ctx
	{
		game_update_dialog* dlg = nullptr;
		std::atomic<bool>* cancel = nullptr;
		std::function<void(u64, u64)> report;
		std::chrono::steady_clock::time_point last{};
	};

	int progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t)
	{
		auto* ctx = static_cast<progress_ctx*>(clientp);

		if (ctx->cancel && *ctx->cancel)
			return 1; // abort transfer

		const auto now = std::chrono::steady_clock::now();
		if (ctx->report && now - ctx->last > std::chrono::milliseconds(200))
		{
			ctx->last = now;
			ctx->report(static_cast<u64>(dlnow), static_cast<u64>(dltotal));
		}

		return 0;
	}
}

game_update_dialog::game_update_dialog(main_window* mw, const std::vector<title_entry>& titles, const std::string& preselect_serial, QWidget* parent)
	: QDialog(parent)
	, m_mw(mw)
	, m_titles(titles)
{
	setWindowTitle(tr("Game Update Downloader"));
	setObjectName("game_update_dialog");
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(620, 420);

	// Title selection
	m_title_box = new QComboBox(this);
	m_title_box->setEditable(true);
	m_title_box->setInsertPolicy(QComboBox::NoInsert);
	m_title_box->lineEdit()->setPlaceholderText(tr("Title ID, e.g. BLUS30443"));

	std::sort(m_titles.begin(), m_titles.end(), [](const title_entry& a, const title_entry& b) { return a.name < b.name; });

	int preselect_index = -1;
	for (const title_entry& t : m_titles)
	{
		if (t.serial == preselect_serial)
			preselect_index = m_title_box->count();

		m_title_box->addItem(QString::fromStdString(t.serial + "  -  " + t.name), QString::fromStdString(t.serial));
	}

	m_title_box->setCurrentIndex(preselect_index);
	if (preselect_index < 0)
		m_title_box->setEditText(QString::fromStdString(preselect_serial));

	m_check_button = new QPushButton(tr("Check for Updates"), this);
	m_check_button->setAutoDefault(false);

	QHBoxLayout* top = new QHBoxLayout();
	top->addWidget(m_title_box, 1);
	top->addWidget(m_check_button);

	m_title_label = new QLabel(tr("Pick a game from your list or type its Title ID, then press \"Check for Updates\"."), this);
	m_title_label->setWordWrap(true);

	// Update list
	m_table = new QTableWidget(0, col_count, this);
	m_table->setHorizontalHeaderLabels({ tr("Version"), tr("Size"), tr("Requires FW"), tr("Status") });
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->verticalHeader()->setVisible(false);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setFocusPolicy(Qt::NoFocus);
	m_table->setAlternatingRowColors(true);

	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setValue(0);
	m_progress->setTextVisible(false);
	m_progress->setVisible(false);

	m_status_label = new QLabel(this);
	m_status_label->setWordWrap(true);

	QPushButton* folder_button = new QPushButton(tr("Open Download Folder"), this);
	folder_button->setAutoDefault(false);

	m_install_button = new QPushButton(tr("Download && Install"), this);
	m_install_button->setDefault(true);
	m_install_button->setEnabled(false);

	m_close_button = new QPushButton(tr("Close"), this);
	m_close_button->setAutoDefault(false);

	QHBoxLayout* bottom = new QHBoxLayout();
	bottom->addWidget(folder_button);
	bottom->addStretch(1);
	bottom->addWidget(m_close_button);
	bottom->addWidget(m_install_button);

	QVBoxLayout* layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(m_title_label);
	layout->addWidget(m_table, 1);
	layout->addWidget(m_progress);
	layout->addWidget(m_status_label);
	layout->addLayout(bottom);
	setLayout(layout);

	connect(m_check_button, &QPushButton::clicked, this, [this]() { check_updates(); });
	connect(m_title_box->lineEdit(), &QLineEdit::returnPressed, this, [this]() { check_updates(); });
	connect(m_install_button, &QPushButton::clicked, this, [this]() { download_and_install(); });
	connect(m_close_button, &QPushButton::clicked, this, [this]() { reject(); });
	connect(folder_button, &QPushButton::clicked, this, []()
	{
		const std::string dir = fs::get_cache_dir() + "game_updates/";
		fs::create_path(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(dir)));
	});

	if (!preselect_serial.empty())
	{
		check_updates();
	}
}

game_update_dialog::~game_update_dialog()
{
	m_cancel = true;
	join_worker();
}

void game_update_dialog::join_worker()
{
	if (m_worker.joinable())
	{
		m_worker.join();
	}
}

void game_update_dialog::reject()
{
	if (m_busy)
	{
		// Cancel the running transfer first; the dialog closes once the worker has stopped.
		m_cancel = true;
		m_close_requested = true;
		set_status(tr("Cancelling..."));
		return;
	}

	QDialog::reject();
}

std::string game_update_dialog::current_serial() const
{
	const int index = m_title_box->currentIndex();

	if (index >= 0 && m_title_box->currentText() == m_title_box->itemText(index))
	{
		return m_title_box->itemData(index).toString().toStdString();
	}

	// Free text: take the first word and normalize it
	std::string text = m_title_box->currentText().trimmed().toUpper().toStdString();
	text = text.substr(0, text.find_first_of(" -"));
	text.erase(std::remove(text.begin(), text.end(), '-'), text.end());
	return text;
}

std::string game_update_dialog::installed_version(const std::string& serial) const
{
	// Installed updates (and HDD games) live in dev_hdd0/game/<serial>/
	const std::string sfo_path = rpcs3::utils::get_hdd0_dir() + "game/" + serial + "/PARAM.SFO";

	if (fs::is_file(sfo_path))
	{
		const psf::registry sfo = psf::load_object(sfo_path);
		const std::string ver(psf::get_string(sfo, "APP_VER"));

		if (!ver.empty())
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

void game_update_dialog::set_busy(bool busy, const QString& status)
{
	m_busy = busy;
	m_title_box->setEnabled(!busy);
	m_check_button->setEnabled(!busy);
	m_install_button->setEnabled(!busy && !m_packages.empty());
	m_close_button->setText(busy ? tr("Cancel") : tr("Close"));
	m_progress->setVisible(busy);

	if (!status.isEmpty())
		set_status(status);
}

void game_update_dialog::check_updates()
{
	if (m_busy)
		return;

	const std::string serial = current_serial();

	if (!is_valid_serial(serial))
	{
		set_status(tr("\"%0\" is not a valid PS3 Title ID (expected 4 letters + 5 digits, e.g. BLUS30443).").arg(QString::fromStdString(serial)));
		return;
	}

	join_worker();

	m_serial = serial;
	m_packages.clear();
	m_table->setRowCount(0);
	m_cancel = false;
	m_progress->setRange(0, 0); // busy indicator
	set_busy(true, tr("Contacting the PS3 update server for %0...").arg(QString::fromStdString(serial)));

	m_worker = std::thread([this, serial]()
	{
		std::string data;
		std::string error;
		std::string title;
		std::vector<update_package> packages;

		rpcs3::curl::curl_handle curl;
		// Sony's public PS3 title-patch server
		const std::string url = fmt::format("https://a0.ww.np.dl.playstation.net/tpl/np/%s/%s-ver.xml", serial, serial);
		game_update_log.notice("Checking for updates: %s", url);

		apply_common_curl_options(curl);
		curl_easy_setopt(curl.get_curl(), CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl.get_curl(), CURLOPT_WRITEFUNCTION, write_string_cb);
		curl_easy_setopt(curl.get_curl(), CURLOPT_WRITEDATA, &data);
		curl_easy_setopt(curl.get_curl(), CURLOPT_TIMEOUT, 30L);
		// The title-patch server uses a certificate from Sony's private CA, which no PC trusts.
		// Only the small version list is fetched here; every PKG is checked against its SHA-1 before install.
		curl_easy_setopt(curl.get_curl(), CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl.get_curl(), CURLOPT_SSL_VERIFYHOST, 0L);

		const CURLcode res = curl_easy_perform(curl.get_curl());
		long http_code = 0;
		curl_easy_getinfo(curl.get_curl(), CURLINFO_RESPONSE_CODE, &http_code);

		if (res != CURLE_OK && http_code != 404)
		{
			error = curl.get_verbose_error(res);
		}
		else if (http_code == 200 && !data.empty())
		{
			QXmlStreamReader xml(QByteArray::fromStdString(data));

			while (!xml.atEnd() && !xml.hasError())
			{
				xml.readNext();

				if (!xml.isStartElement())
					continue;

				if (xml.name() == u"package")
				{
					const QXmlStreamAttributes attr = xml.attributes();
					update_package pkg;
					pkg.version = attr.value("version").toString().toStdString();
					pkg.size = attr.value("size").toString().toULongLong();
					pkg.sha1 = attr.value("sha1sum").toString().toLower().toStdString();
					pkg.url = attr.value("url").toString().toStdString();
					pkg.required_fw = attr.value("ps3_system_ver").toString().toStdString();

					if (!pkg.url.empty())
						packages.push_back(std::move(pkg));
				}
				else if (xml.name() == u"TITLE")
				{
					// Keep the last title (newest package carries the current name)
					title = xml.readElementText().simplified().toStdString();
				}
			}

			if (xml.hasError() && packages.empty())
			{
				error = "Could not read the update list: " + xml.errorString().toStdString();
			}
		}

		std::sort(packages.begin(), packages.end(), [](const update_package& a, const update_package& b)
		{
			return version_number(a.version) < version_number(b.version);
		});

		QMetaObject::invokeMethod(this, [this, serial, title, packages, error]()
		{
			on_check_finished(serial, title, packages, error);
		}, Qt::QueuedConnection);
	});
}

void game_update_dialog::on_check_finished(const std::string& serial, const std::string& title, const std::vector<update_package>& packages, const std::string& error)
{
	join_worker();
	set_busy(false);

	if (m_close_requested)
	{
		QDialog::reject();
		return;
	}

	if (!error.empty())
	{
		game_update_log.error("Update check for %s failed: %s", serial, error);
		set_status(tr("Could not reach the update server.\n%0").arg(QString::fromStdString(error)));
		return;
	}

	const std::string installed = installed_version(serial);
	const double installed_num = installed.empty() ? 0.0 : version_number(installed);

	QString name = QString::fromStdString(title.empty() ? serial : title + " (" + serial + ")");
	if (!installed.empty())
		name += tr("  -  installed version %0").arg(QString::fromStdString(installed));
	m_title_label->setText(name);

	m_packages.clear();
	m_table->setRowCount(0);

	if (packages.empty())
	{
		set_status(tr("No updates are available for %0.").arg(QString::fromStdString(serial)));
		m_install_button->setEnabled(false);
		return;
	}

	int pending = 0;

	for (const update_package& pkg : packages)
	{
		const bool already = installed_num > 0.0 && version_number(pkg.version) <= installed_num;
		const int row = m_table->rowCount();
		m_table->insertRow(row);

		QTableWidgetItem* ver_item = new QTableWidgetItem(QString::fromStdString(pkg.version));
		ver_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
		ver_item->setCheckState(already ? Qt::Unchecked : Qt::Checked);

		m_table->setItem(row, col_version, ver_item);
		m_table->setItem(row, col_size, new QTableWidgetItem(format_size(pkg.size)));
		m_table->setItem(row, col_firmware, new QTableWidgetItem(format_fw(pkg.required_fw)));
		m_table->setItem(row, col_status, new QTableWidgetItem(already ? tr("Installed") : tr("Available")));

		if (!already)
			pending++;
	}

	m_packages = packages;
	m_install_button->setEnabled(true);
	set_status(pending
		? tr("%0 update(s) available. Updates install in order, oldest first.").arg(pending)
		: tr("This game is already up to date. You can still tick a version to reinstall it."));
}

void game_update_dialog::download_and_install()
{
	if (m_busy || m_packages.empty())
		return;

	std::vector<update_package> selected;
	std::vector<int> rows;

	for (int row = 0; row < m_table->rowCount() && row < static_cast<int>(m_packages.size()); row++)
	{
		if (m_table->item(row, col_version)->checkState() == Qt::Checked)
		{
			selected.push_back(m_packages[row]);
			rows.push_back(row);
		}
	}

	if (selected.empty())
	{
		set_status(tr("Tick at least one version to download."));
		return;
	}

	if (!Emu.IsStopped())
	{
		QMessageBox::warning(this, tr("Emulation running"), tr("Stop the running game before installing updates."));
		return;
	}

	join_worker();

	const std::string dir = fs::get_cache_dir() + "game_updates/" + m_serial + "/";
	fs::create_path(dir);

	m_cancel = false;
	m_progress->setRange(0, 1000);
	m_progress->setValue(0);
	set_busy(true, tr("Starting download..."));

	m_worker = std::thread([this, selected, rows, dir]()
	{
		std::vector<std::string> files;
		std::string error;

		for (usz i = 0; i < selected.size() && error.empty() && !m_cancel; i++)
		{
			const update_package& pkg = selected[i];
			const int row = rows[i];
			const std::string file_name = pkg.url.substr(pkg.url.find_last_of('/') + 1);
			const std::string path = dir + (file_name.empty() ? pkg.version + ".pkg" : file_name);

			const auto set_row_status = [this, row](const QString& text)
			{
				QMetaObject::invokeMethod(this, [this, row, text]()
				{
					if (QTableWidgetItem* item = m_table->item(row, col_status))
						item->setText(text);
				}, Qt::QueuedConnection);
			};

			// Reuse a previous complete download if it still matches the expected hash
			if (fs::is_file(path) && !pkg.sha1.empty())
			{
				pkg_hasher existing;
				fs::file f(path);
				std::vector<char> buf(4 * 1024 * 1024);
				u64 read = 0;
				while (f && (read = f.read(buf.data(), buf.size())) > 0 && !m_cancel)
					existing.add(buf.data(), read);

				if (!m_cancel && existing.matches(pkg.sha1))
				{
					game_update_log.notice("Using cached update %s", path);
					set_row_status(tr("Downloaded"));
					files.push_back(path);
					continue;
				}
			}

			game_update_log.notice("Downloading update %s -> %s", pkg.url, path);
			set_row_status(tr("Downloading..."));

			file_download_ctx dl;
			dl.file.open(path, fs::rewrite);

			if (!dl.file)
			{
				error = "Could not create " + path;
				break;
			}

			const QString label = tr("Downloading %0 (%1 of %2)").arg(QString::fromStdString(pkg.version)).arg(i + 1).arg(selected.size());

			progress_ctx prog;
			prog.dlg = this;
			prog.cancel = &m_cancel;
			prog.report = [this, label, expected = pkg.size](u64 now, u64 total)
			{
				if (!total)
					total = expected;

				QMetaObject::invokeMethod(this, [this, label, now, total]()
				{
					m_progress->setValue(total ? static_cast<int>(now * 1000 / total) : 0);
					set_status(label + QStringLiteral("  -  ") + format_size(now) + QStringLiteral(" / ") + format_size(total));
				}, Qt::QueuedConnection);
			};

			rpcs3::curl::curl_handle curl;
			apply_common_curl_options(curl);
			curl_easy_setopt(curl.get_curl(), CURLOPT_URL, pkg.url.c_str());
			curl_easy_setopt(curl.get_curl(), CURLOPT_WRITEFUNCTION, write_file_cb);
			curl_easy_setopt(curl.get_curl(), CURLOPT_WRITEDATA, &dl);
			curl_easy_setopt(curl.get_curl(), CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl.get_curl(), CURLOPT_XFERINFOFUNCTION, progress_cb);
			curl_easy_setopt(curl.get_curl(), CURLOPT_XFERINFODATA, &prog);

			const CURLcode res = curl_easy_perform(curl.get_curl());
			dl.file.close();

			if (m_cancel)
			{
				fs::remove_file(path);
				set_row_status(tr("Cancelled"));
				break;
			}

			if (res != CURLE_OK)
			{
				fs::remove_file(path);
				error = fmt::format("Download of version %s failed: %s", pkg.version, curl.get_verbose_error(res));
				set_row_status(tr("Failed"));
				break;
			}

			const std::string hash = dl.hash.full_hex();

			if (!pkg.sha1.empty() && !dl.hash.matches(pkg.sha1))
			{
				fs::remove_file(path);
				error = fmt::format("Version %s failed verification (SHA-1 %s, expected %s). The file was deleted.", pkg.version, hash, pkg.sha1);
				set_row_status(tr("Corrupt"));
				break;
			}

			set_row_status(tr("Downloaded"));
			files.push_back(path);
		}

		const bool cancelled = m_cancel;

		QMetaObject::invokeMethod(this, [this, files, error, cancelled]()
		{
			on_downloads_finished(files, error, cancelled);
		}, Qt::QueuedConnection);
	});
}

void game_update_dialog::on_downloads_finished(const std::vector<std::string>& files, const std::string& error, bool cancelled)
{
	join_worker();
	set_busy(false);

	if (m_close_requested)
	{
		QDialog::reject();
		return;
	}

	if (cancelled)
	{
		set_status(tr("Download cancelled."));
		return;
	}

	if (!error.empty())
	{
		game_update_log.error("%s", error);
		set_status(QString::fromStdString(error));

		if (files.empty())
			return;
	}

	if (files.empty())
		return;

	// Install in version order through the standard package installer (handles progress, errors and game list refresh)
	QStringList paths;
	for (const std::string& file : files)
		paths << QString::fromStdString(file);

	set_status(tr("Installing %0 package(s)...").arg(paths.size()));

	if (main_window::InstallPackages(m_mw, paths))
	{
		set_status(tr("Updates installed. They are kept in the download folder in case you need to reinstall."));
		check_updates(); // refresh installed state
	}
	else
	{
		set_status(tr("Installation did not complete. See the log for details."));
	}
}
