#pragma once

// RPCS3 Lite: Game Update Downloader
// Looks up official title updates on Sony's PS3 update server for a Title ID,
// downloads the PKG files (SHA-1 verified) and installs them with the regular package installer.

#include "util/types.hpp"

#include <QDialog>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class main_window;

class game_update_dialog final : public QDialog
{
public:
	struct title_entry
	{
		std::string serial;
		std::string name;
		std::string app_ver;
	};

	struct update_package
	{
		std::string version;
		u64 size = 0;
		std::string sha1;
		std::string url;
		std::string required_fw;
	};

	game_update_dialog(main_window* mw, const std::vector<title_entry>& titles, const std::string& preselect_serial, QWidget* parent);
	~game_update_dialog() override;

	void reject() override;

private:
	void check_updates();
	void download_and_install();
	void on_check_finished(const std::string& serial, const std::string& title, const std::vector<update_package>& packages, const std::string& error);
	void on_downloads_finished(const std::vector<std::string>& files, const std::string& error, bool cancelled);
	void set_busy(bool busy, const QString& status = {});
	void set_status(const QString& text);
	std::string current_serial() const;
	std::string installed_version(const std::string& serial) const;
	void join_worker();

	main_window* m_mw = nullptr;
	std::vector<title_entry> m_titles;
	std::vector<update_package> m_packages;
	std::string m_serial;

	QComboBox* m_title_box = nullptr;
	QPushButton* m_check_button = nullptr;
	QLabel* m_title_label = nullptr;
	QTableWidget* m_table = nullptr;
	QProgressBar* m_progress = nullptr;
	QLabel* m_status_label = nullptr;
	QPushButton* m_install_button = nullptr;
	QPushButton* m_close_button = nullptr;

	std::thread m_worker;
	std::atomic<bool> m_cancel{false};
	bool m_busy = false;
	bool m_close_requested = false;
};
