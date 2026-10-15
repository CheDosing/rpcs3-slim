#pragma once

// RPCS3 Slim: Game Update Downloader
// Front end for the bundled rusty-psn CLI (https://github.com/RainbowCookie32/rusty-psn, MIT). rusty-psn runs hidden
// in its --json mode; this dialog lists the updates it finds, lets the user pick them, shows download progress and
// installs the verified PKGs with the regular package installer.

#include <QDialog>
#include <QStringList>

#include <map>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QJsonObject;
class QLabel;
class QProcess;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
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

	game_update_dialog(main_window* mw, const QString& rusty_psn_path, std::vector<title_entry> titles, const std::string& preselect_serial, QWidget* parent);
	~game_update_dialog() override;

	// Path of rusty-psn-cli: next to rpcs3 (portable builds), <prefix>/lib/rpcs3-slim (Linux packages) or PATH
	static QString find_rusty_psn();

	void reject() override;

private:
	enum class phase
	{
		idle,
		checking,
		downloading,
	};

	struct package_row
	{
		QString title_id;
		int index = 0;
		quint64 size = 0;
		QString version;
		QTreeWidgetItem* item = nullptr;
		QProgressBar* bar = nullptr;
	};

	void check_updates();
	void download_selected();
	void start_process(const QStringList& args, phase new_phase);
	void on_output();
	void on_event(const QJsonObject& obj);
	void on_process_finished();
	void install_finished();

	void add_title(const QJsonObject& obj);
	package_row* find_row(const QString& title_id, int index);
	void set_row_status(package_row& row, const QString& text);
	void update_controls();
	void update_overall_progress();
	void set_status(const QString& text);
	QStringList requested_serials() const;
	std::string installed_version(const std::string& serial) const;
	QString download_dir() const;

	main_window* m_mw = nullptr;
	QString m_rusty_psn;
	std::vector<title_entry> m_titles;

	QComboBox* m_title_box = nullptr;
	QPushButton* m_check_button = nullptr;
	QLabel* m_header_label = nullptr;
	QTreeWidget* m_tree = nullptr;
	QProgressBar* m_progress = nullptr;
	QLabel* m_status_label = nullptr;
	QCheckBox* m_install_check = nullptr;
	QPushButton* m_download_button = nullptr;
	QPushButton* m_close_button = nullptr;

	QProcess* m_process = nullptr;
	QByteArray m_output;
	phase m_phase = phase::idle;
	bool m_cancelled = false;
	bool m_close_requested = false;

	std::vector<package_row> m_rows;
	std::map<QString, QTreeWidgetItem*> m_title_items;
	QStringList m_errors;

	// Download run state
	quint64 m_run_total = 0;
	quint64 m_run_done = 0;      // bytes of packages already finished in this run
	quint64 m_current_bytes = 0; // bytes of the package being downloaded
	int m_run_count = 0;
	int m_run_index = 0;
	int m_run_failed = 0;
	QStringList m_finished_paths;
};
