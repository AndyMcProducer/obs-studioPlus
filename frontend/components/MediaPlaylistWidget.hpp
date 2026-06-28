#pragma once

#include <obs.hpp>

#include <QHash>
#include <QPixmap>
#include <QUrl>
#include <QWidget>

class QLabel;
class QListWidget;
class QListWidgetItem;
class MediaControls;
class QNetworkAccessManager;
class QNetworkReply;

class MediaPlaylistWidget : public QWidget {
	Q_OBJECT

private:
	std::vector<OBSSignal> sigs;
	OBSWeakSource weakSource = nullptr;
	QString noSourceText;
	QLabel *sourceLabel = nullptr;
	QLabel *emptyLabel = nullptr;
	QListWidget *playlist = nullptr;
	MediaControls *controls = nullptr;
	QNetworkAccessManager *network = nullptr;
	QHash<QString, QPixmap> thumbnailCache;
	QHash<QString, QString> metadataTitleCache;
	QHash<QString, QString> metadataThumbnailCache;

	bool IsSupportedSource(OBSSource source) const;
	QString GetDisplayPath(const QString &path) const;
	QString GetDisplayInfo(const QString &path) const;
	QString GetNoSourceText() const;
	QUrl GetThumbnailUrl(const QString &path) const;
	QUrl GetMetadataUrl(const QString &path, QString &key) const;
	void RequestThumbnail(QListWidgetItem *item, const QUrl &url);
	void ApplyThumbnail(const QString &url, const QPixmap &pixmap);
	void RequestMetadata(QListWidgetItem *item, const QString &path);
	void ApplyMetadata(const QString &key);
	void SetItemText(QListWidgetItem *item, const QString &title, const QString &info) const;

	static void OBSPlaylistUpdated(void *data, calldata_t *calldata);
	static void OBSPlaylistSelectionChanged(void *data, calldata_t *calldata);
	static void OBSSourceRenamed(void *data, calldata_t *calldata);

private slots:
	void UpdatePlaylist();
	void UpdateSourceName();
	void UpdateCurrentSelection();
	void ActivateSelectedItem(QListWidgetItem *item);

public:
	explicit MediaPlaylistWidget(QWidget *parent = nullptr);
	~MediaPlaylistWidget();

	void SetNoSourceText(const QString &text);
	void SetSource(OBSSource source);
	OBSSource GetSource();
};
