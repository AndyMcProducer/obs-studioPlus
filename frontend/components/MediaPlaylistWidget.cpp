#include "MediaPlaylistWidget.hpp"

#include "MediaControls.hpp"

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QFileInfo>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSize>
#include <QVBoxLayout>
#include <QUrl>
#include <QUrlQuery>

#include "moc_MediaPlaylistWidget.cpp"

MediaPlaylistWidget::MediaPlaylistWidget(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *mainLayout = new QVBoxLayout(this);
	QFont titleFont;

	mainLayout->setContentsMargins(8, 8, 8, 8);
	mainLayout->setSpacing(8);

	sourceLabel = new QLabel(GetNoSourceText(), this);
	titleFont = sourceLabel->font();
	titleFont.setBold(true);
	sourceLabel->setFont(titleFont);
	sourceLabel->setWordWrap(true);
	mainLayout->addWidget(sourceLabel);

	QLabel *playlistLabel = new QLabel(QTStr("MediaPlaylistDock.Playlist"), this);
	mainLayout->addWidget(playlistLabel);

	playlist = new QListWidget(this);
	playlist->setSelectionMode(QAbstractItemView::SingleSelection);
	playlist->setAlternatingRowColors(true);
	playlist->setIconSize(QSize(96, 54));
	playlist->hide();
	mainLayout->addWidget(playlist, 1);

	emptyLabel = new QLabel(GetNoSourceText(), this);
	emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLabel->setWordWrap(true);
	mainLayout->addWidget(emptyLabel, 1);

	controls = new MediaControls(this);
	controls->SetPlayPauseButtonAfterStop(true);
	mainLayout->addWidget(controls);

	network = new QNetworkAccessManager(this);

	connect(playlist, &QListWidget::itemActivated, this, &MediaPlaylistWidget::ActivateSelectedItem);
}

MediaPlaylistWidget::~MediaPlaylistWidget() {}

bool MediaPlaylistWidget::IsSupportedSource(OBSSource source) const
{
	if (!source)
		return false;

	const char *id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, "ffmpeg_source") == 0 || strcmp(id, "browser_playlist_source") == 0);
}

QString MediaPlaylistWidget::GetDisplayPath(const QString &path) const
{
	QUrl url(path);

	if (url.isValid() && !url.scheme().isEmpty()) {
		if (!url.host().isEmpty() && url.path() != QLatin1String("/"))
			return QStringLiteral("%1%2").arg(url.host(), url.path());

		if (!url.host().isEmpty())
			return url.host();
	}

	QString displayPath = QFileInfo(path).fileName();

	if (!displayPath.isEmpty())
		return displayPath;

	displayPath = QFileInfo(url.path()).fileName();
	return displayPath.isEmpty() ? path : displayPath;
}

static QString youtube_video_id(const QUrl &url)
{
	QString host = url.host().toLower();
	QString path = url.path();

	if (host == QLatin1String("youtu.be") || host.endsWith(QLatin1String(".youtu.be"))) {
		QStringList parts = path.split('/', Qt::SkipEmptyParts);
		return parts.isEmpty() ? QString() : parts.first();
	}

	if (host == QLatin1String("youtube.com") || host.endsWith(QLatin1String(".youtube.com")) ||
	    host == QLatin1String("yout-ube.com") || host.endsWith(QLatin1String(".yout-ube.com"))) {
		QUrlQuery query(url);
		QString id = query.queryItemValue(QStringLiteral("v"));

		if (!id.isEmpty())
			return id;

		QStringList parts = path.split('/', Qt::SkipEmptyParts);
		if (parts.size() >= 2 &&
		    (parts.first() == QLatin1String("embed") || parts.first() == QLatin1String("shorts")))
			return parts.at(1);
	}

	return {};
}

QString MediaPlaylistWidget::GetDisplayInfo(const QString &path) const
{
	QUrl url(path);
	QString videoId = youtube_video_id(url);

	if (!videoId.isEmpty())
		return QStringLiteral("YouTube video %1").arg(videoId);

	if (url.isValid() && !url.host().isEmpty())
		return url.host();

	return QFileInfo(path).absolutePath();
}

QString MediaPlaylistWidget::GetNoSourceText() const
{
	return noSourceText.isEmpty() ? QTStr("MediaPlaylistDock.NoSource") : noSourceText;
}

QUrl MediaPlaylistWidget::GetThumbnailUrl(const QString &path) const
{
	QUrl url(path);
	QString videoId = youtube_video_id(url);

	if (!videoId.isEmpty())
		return QUrl(QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(videoId));

	if (url.isValid() && !url.scheme().isEmpty() && !url.host().isEmpty())
		return QUrl(QStringLiteral("%1://%2/favicon.ico").arg(url.scheme(), url.host()));

	return {};
}

QUrl MediaPlaylistWidget::GetMetadataUrl(const QString &path, QString &key) const
{
	QUrl url(path);
	QString videoId = youtube_video_id(url);

	if (videoId.isEmpty())
		return {};

	key = QStringLiteral("youtube:%1").arg(videoId);

	QUrl metadataUrl(QStringLiteral("https://www.youtube.com/oembed"));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("url"), QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoId));
	query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
	metadataUrl.setQuery(query);
	return metadataUrl;
}

void MediaPlaylistWidget::SetItemText(QListWidgetItem *item, const QString &title, const QString &info) const
{
	if (!item)
		return;

	item->setText(QStringLiteral("%1. %2\n%3").arg(playlist->row(item) + 1).arg(title, info));
}

void MediaPlaylistWidget::RequestThumbnail(QListWidgetItem *item, const QUrl &url)
{
	if (!item || !url.isValid() || url.isEmpty())
		return;

	QString key = url.toString();
	item->setData(Qt::UserRole + 1, key);

	if (thumbnailCache.contains(key)) {
		item->setIcon(QIcon(thumbnailCache.value(key)));
		return;
	}

	QNetworkReply *reply = network->get(QNetworkRequest(url));
	connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
		QPixmap pixmap;

		if (reply->error() == QNetworkReply::NoError && pixmap.loadFromData(reply->readAll())) {
			pixmap = pixmap.scaled(96, 54, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			thumbnailCache.insert(key, pixmap);
			ApplyThumbnail(key, pixmap);
		}

		reply->deleteLater();
	});
}

void MediaPlaylistWidget::RequestMetadata(QListWidgetItem *item, const QString &path)
{
	QString key;
	QUrl metadataUrl = GetMetadataUrl(path, key);

	if (!item || key.isEmpty() || !metadataUrl.isValid())
		return;

	item->setData(Qt::UserRole + 2, key);

	if (metadataTitleCache.contains(key)) {
		ApplyMetadata(key);
		return;
	}

	QNetworkReply *reply = network->get(QNetworkRequest(metadataUrl));
	connect(reply, &QNetworkReply::finished, this, [this, reply, key]() {
		if (reply->error() == QNetworkReply::NoError) {
			QJsonParseError error;
			QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &error);

			if (error.error == QJsonParseError::NoError && doc.isObject()) {
				QJsonObject object = doc.object();
				QString title = object.value(QStringLiteral("title")).toString();
				QString thumbnailUrl = object.value(QStringLiteral("thumbnail_url")).toString();

				if (!title.isEmpty())
					metadataTitleCache.insert(key, title);
				if (!thumbnailUrl.isEmpty())
					metadataThumbnailCache.insert(key, thumbnailUrl);
			}
		}

		ApplyMetadata(key);
		reply->deleteLater();
	});
}

void MediaPlaylistWidget::ApplyThumbnail(const QString &url, const QPixmap &pixmap)
{
	for (int i = 0; i < playlist->count(); i++) {
		QListWidgetItem *item = playlist->item(i);

		if (item && item->data(Qt::UserRole + 1).toString() == url)
			item->setIcon(QIcon(pixmap));
	}
}

void MediaPlaylistWidget::ApplyMetadata(const QString &key)
{
	const QString title = metadataTitleCache.value(key);
	const QString thumbnailUrl = metadataThumbnailCache.value(key);

	for (int i = 0; i < playlist->count(); i++) {
		QListWidgetItem *item = playlist->item(i);

		if (!item || item->data(Qt::UserRole + 2).toString() != key)
			continue;

		if (!title.isEmpty())
			SetItemText(item, title, item->data(Qt::UserRole + 3).toString());

		if (!thumbnailUrl.isEmpty())
			RequestThumbnail(item, QUrl(thumbnailUrl));
	}
}

void MediaPlaylistWidget::OBSPlaylistUpdated(void *data, calldata_t *)
{
	MediaPlaylistWidget *widget = static_cast<MediaPlaylistWidget *>(data);
	QMetaObject::invokeMethod(widget, "UpdatePlaylist", Qt::QueuedConnection);
}

void MediaPlaylistWidget::OBSPlaylistSelectionChanged(void *data, calldata_t *)
{
	MediaPlaylistWidget *widget = static_cast<MediaPlaylistWidget *>(data);
	QMetaObject::invokeMethod(widget, "UpdateCurrentSelection", Qt::QueuedConnection);
}

void MediaPlaylistWidget::OBSSourceRenamed(void *data, calldata_t *)
{
	MediaPlaylistWidget *widget = static_cast<MediaPlaylistWidget *>(data);
	QMetaObject::invokeMethod(widget, "UpdateSourceName", Qt::QueuedConnection);
}

void MediaPlaylistWidget::UpdatePlaylist()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	QSignalBlocker signalBlocker(playlist);
	int count = 0;

	UpdateSourceName();
	controls->SetSource(IsSupportedSource(source) ? source : nullptr);
	playlist->clear();

	if (!IsSupportedSource(source)) {
		emptyLabel->setText(GetNoSourceText());
		emptyLabel->show();
		playlist->hide();
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	if (ph) {
		calldata_t cd = {};

		if (proc_handler_call(ph, "get_playlist_count", &cd))
			count = (int)calldata_int(&cd, "count");

		calldata_free(&cd);
	}

	if (count <= 0) {
		emptyLabel->setText(QTStr("MediaPlaylistDock.NoItems"));
		emptyLabel->show();
		playlist->hide();
		return;
	}

	for (int i = 0; i < count; i++) {
		calldata_t cd = {};
		QString path;
		QString displayPath;
		QString displayInfo;

		calldata_set_int(&cd, "index", i);
		if (proc_handler_call(ph, "get_playlist_item", &cd))
			path = QT_UTF8(calldata_string(&cd, "path"));
		calldata_free(&cd);

		displayPath = GetDisplayPath(path);
		displayInfo = GetDisplayInfo(path);

		QListWidgetItem *item = new QListWidgetItem;
		item->setData(Qt::UserRole, path);
		item->setData(Qt::UserRole + 3, displayPath);
		item->setToolTip(path);
		playlist->addItem(item);
		SetItemText(item, displayPath, displayInfo);
		RequestThumbnail(item, GetThumbnailUrl(path));
		RequestMetadata(item, path);
	}

	emptyLabel->hide();
	playlist->show();
	UpdateCurrentSelection();
}

void MediaPlaylistWidget::UpdateSourceName()
{
	OBSSource source = OBSGetStrongRef(weakSource);

	if (!IsSupportedSource(source)) {
		sourceLabel->setText(GetNoSourceText());
		return;
	}

	sourceLabel->setText(QT_UTF8(obs_source_get_name(source)));
}

void MediaPlaylistWidget::UpdateCurrentSelection()
{
	OBSSource source = OBSGetStrongRef(weakSource);
	QSignalBlocker signalBlocker(playlist);
	int index = -1;

	if (!IsSupportedSource(source) || playlist->count() == 0) {
		playlist->clearSelection();
		return;
	}

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	if (ph) {
		calldata_t cd = {};

		if (proc_handler_call(ph, "get_playlist_index", &cd))
			index = (int)calldata_int(&cd, "index");

		calldata_free(&cd);
	}

	if (index >= 0 && index < playlist->count()) {
		playlist->setCurrentRow(index);
		playlist->scrollToItem(playlist->item(index), QAbstractItemView::PositionAtCenter);
	} else {
		playlist->clearSelection();
	}
}

void MediaPlaylistWidget::ActivateSelectedItem(QListWidgetItem *item)
{
	OBSSource source = OBSGetStrongRef(weakSource);
	int index;

	if (!IsSupportedSource(source))
		return;

	index = item ? playlist->row(item) : playlist->currentRow();
	if (index < 0)
		return;

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	if (!ph)
		return;

	calldata_t cd = {};
	calldata_set_int(&cd, "index", index);
	proc_handler_call(ph, "set_playlist_index", &cd);
	calldata_free(&cd);
}

void MediaPlaylistWidget::SetNoSourceText(const QString &text)
{
	if (noSourceText == text)
		return;

	noSourceText = text;
	UpdatePlaylist();
}

void MediaPlaylistWidget::SetSource(OBSSource source)
{
	if (GetSource() == source) {
		UpdatePlaylist();
		return;
	}

	sigs.clear();
	weakSource = nullptr;

	if (IsSupportedSource(source)) {
		weakSource = OBSGetWeakRef(source);

		signal_handler_t *sh = obs_source_get_signal_handler(source);
		sigs.emplace_back(sh, "rename", OBSSourceRenamed, this);
		sigs.emplace_back(sh, "playlist_updated", OBSPlaylistUpdated, this);
		sigs.emplace_back(sh, "playlist_selection_changed", OBSPlaylistSelectionChanged, this);
	}

	UpdatePlaylist();
}

OBSSource MediaPlaylistWidget::GetSource()
{
	return OBSGetStrongRef(weakSource);
}
