/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <components/MediaPlaylistWidget.hpp>

#include <json11.hpp>

#include <qt-wrappers.hpp>

static bool is_media_playlist_source(obs_source_t *source)
{
	const char *id;

	if (!source)
		return false;

	id = obs_source_get_unversioned_id(source);
	return id && (strcmp(id, "ffmpeg_source") == 0 || strcmp(id, "browser_playlist_source") == 0);
}

static QString get_media_playlist_source_uuid(obs_source_t *source)
{
	const char *uuid;

	if (!source)
		return {};

	uuid = obs_source_get_uuid(source);
	return uuid && *uuid ? QT_UTF8(uuid) : QString();
}

static QString get_media_playlist_dock_object_name(const QString &sourceUuid)
{
	QString sanitizedUuid = sourceUuid;
	sanitizedUuid.remove('{');
	sanitizedUuid.remove('}');
	sanitizedUuid.replace('-', '_');
	return QStringLiteral("mediaPlaylistSourceDock_%1").arg(sanitizedUuid);
}

static QString get_media_playlist_dock_title(obs_source_t *source)
{
	if (!source)
		return QTStr("MediaPlaylistDock.Title");

	return QStringLiteral("%1: %2").arg(QTStr("MediaPlaylistDock.Title"), QT_UTF8(obs_source_get_name(source)));
}

void OBSBasic::AddMediaPlaylistSourceDock(const QString &sourceUuid, bool show)
{
	OBSDock *existingDock = nullptr;
	MediaPlaylistWidget *widget;
	OBSDock *dock;
	QString objectName;
	OBSSourceAutoRelease source;

	if (sourceUuid.isEmpty())
		return;

	for (auto &candidate : mediaPlaylistSourceDocks) {
		if (candidate && candidate->property("media_playlist_source_uuid").toString() == sourceUuid) {
			existingDock = candidate;
			break;
		}
	}

	if (existingDock) {
		if (show) {
			existingDock->show();
			existingDock->raise();
		}
		return;
	}

	widget = new MediaPlaylistWidget(this);
	widget->SetNoSourceText(QTStr("MediaPlaylistDock.SourceUnavailable"));

	source = obs_get_source_by_uuid(QT_TO_UTF8(sourceUuid));
	if (source && is_media_playlist_source(source))
		widget->SetSource(source.Get());

	dock = new OBSDock(this);
	objectName = get_media_playlist_dock_object_name(sourceUuid);
	dock->setProperty("media_playlist_source_uuid", sourceUuid);
	dock->setObjectName(objectName);
	dock->setWindowTitle(get_media_playlist_dock_title(source));
	dock->setAllowedAreas(Qt::AllDockWidgetAreas);
	dock->setAttribute(Qt::WA_DeleteOnClose);
	dock->setWidget(widget);

	connect(dock, &QObject::destroyed, this, [this, objectName, sourceUuid]() {
		for (int i = mediaPlaylistSourceDocks.size() - 1; i >= 0; i--) {
			if (mediaPlaylistSourceDocks[i].isNull() || mediaPlaylistSourceDocks[i]->objectName() == objectName)
				mediaPlaylistSourceDocks.removeAt(i);
		}

		mediaPlaylistDockUuids.removeAll(sourceUuid);

		int idx = extraCustomDockNames.indexOf(objectName);
		if (idx != -1) {
			extraCustomDockNames.removeAt(idx);
			extraCustomDocks.removeAt(idx);
		}

		RefreshMediaPlaylistDocks();
	});

	AddCustomDockWidget(dock);
	mediaPlaylistSourceDocks.push_back(dock);
	if (!mediaPlaylistDockUuids.contains(sourceUuid))
		mediaPlaylistDockUuids.push_back(sourceUuid);

	if (show) {
		dock->show();
		dock->raise();
	}

	RefreshMediaPlaylistDocks();
}

void OBSBasic::RemoveMediaPlaylistSourceDock(const QString &sourceUuid)
{
	for (auto &dock : mediaPlaylistSourceDocks) {
		if (dock && dock->property("media_playlist_source_uuid").toString() == sourceUuid) {
			dock->close();
			return;
		}
	}
}

void OBSBasic::LoadMediaPlaylistDocks()
{
	using namespace json11;
	const char *jsonStr = config_get_string(App()->GetUserConfig(), "BasicWindow", "MediaPlaylistDocks");
	std::string err;
	Json json = Json::parse(jsonStr, err);

	if (!err.empty())
		return;

	for (const Json &item : json.array_items()) {
		QString sourceUuid = QT_UTF8(item.string_value().c_str());

		if (!sourceUuid.isEmpty())
			AddMediaPlaylistSourceDock(sourceUuid, false);
	}
}

void OBSBasic::SaveMediaPlaylistDocks()
{
	using namespace json11;
	Json::array array;

	for (auto &dock : mediaPlaylistSourceDocks) {
		if (!dock)
			continue;

		QString sourceUuid = dock->property("media_playlist_source_uuid").toString();
		if (!sourceUuid.isEmpty())
			array.push_back(QT_TO_UTF8(sourceUuid));
	}

	config_set_string(App()->GetUserConfig(), "BasicWindow", "MediaPlaylistDocks", Json(array).dump().c_str());
}

void OBSBasic::RefreshMediaPlaylistDocks()
{
	QMap<QString, QString> sourcesByName;

	for (int i = mediaPlaylistSourceDocks.size() - 1; i >= 0; i--) {
		if (mediaPlaylistSourceDocks[i].isNull())
			mediaPlaylistSourceDocks.removeAt(i);
	}

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			QMap<QString, QString> *sourcesByName = static_cast<QMap<QString, QString> *>(param);
			QString sourceUuid;

			if (!is_media_playlist_source(source))
				return true;

			sourceUuid = get_media_playlist_source_uuid(source);
			if (sourceUuid.isEmpty())
				return true;

			sourcesByName->insert(QT_UTF8(obs_source_get_name(source)), sourceUuid);
			return true;
		},
		&sourcesByName);

	if (mediaPlaylistMenu) {
		mediaPlaylistMenu->clear();

		if (sourcesByName.isEmpty()) {
			QAction *action = mediaPlaylistMenu->addAction(QTStr("MediaPlaylistDock.NoSources"));
			action->setEnabled(false);
		} else {
			for (auto it = sourcesByName.cbegin(); it != sourcesByName.cend(); ++it) {
				QString sourceUuid = it.value();
				QAction *action = mediaPlaylistMenu->addAction(it.key());

				connect(action, &QAction::triggered, this,
					[this, sourceUuid]() { AddMediaPlaylistSourceDock(sourceUuid, true); });
			}
		}
	}

	for (auto &dock : mediaPlaylistSourceDocks) {
		MediaPlaylistWidget *widget;
		QString sourceUuid;
		OBSSourceAutoRelease source;

		if (!dock)
			continue;

		widget = qobject_cast<MediaPlaylistWidget *>(dock->widget());
		if (!widget)
			continue;

		sourceUuid = dock->property("media_playlist_source_uuid").toString();
		source = obs_get_source_by_uuid(QT_TO_UTF8(sourceUuid));

		if (source && is_media_playlist_source(source)) {
			widget->SetSource(source.Get());
			dock->setWindowTitle(get_media_playlist_dock_title(source));
		} else {
			widget->SetSource(nullptr);
			dock->setWindowTitle(QTStr("MediaPlaylistDock.Title"));
		}
	}
}

void setupDockAction(QDockWidget *dock)
{
	QAction *action = dock->toggleViewAction();

	auto neverDisable = [action]() {
		QSignalBlocker block(action);
		action->setEnabled(true);
	};

	auto newToggleView = [dock](bool check) {
		QSignalBlocker block(dock);
		dock->setVisible(check);
	};

	// Replace the slot connected by default
	QObject::disconnect(action, &QAction::triggered, nullptr, 0);
	QObject::connect(action, &QAction::triggered, dock, newToggleView);

	// Make the action unable to be disabled
	QObject::connect(action, &QAction::enabledChanged, action, neverDisable);
}

void OBSBasic::on_resetDocks_triggered(bool force)
{
#ifdef BROWSER_AVAILABLE
	if ((extraDocks.size() || extraCustomDocks.size() || extraBrowserDocks.size()) && !force)
#else
	if ((extraDocks.size() || extraCustomDocks.size()) && !force)
#endif
	{
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("ResetUIWarning.Title"), QTStr("ResetUIWarning.Text"));

		if (button == QMessageBox::No)
			return;
	}

#define RESET_DOCKLIST(dockList)                                                                               \
	for (int i = dockList.size() - 1; i >= 0; i--) {                                                       \
		dockList[i]->setVisible(true);                                                                 \
		dockList[i]->setFloating(true);                                                                \
		dockList[i]->move(frameGeometry().topLeft() + rect().center() - dockList[i]->rect().center()); \
		dockList[i]->setVisible(false);                                                                \
	}

	RESET_DOCKLIST(extraDocks)
	RESET_DOCKLIST(extraCustomDocks)
#ifdef BROWSER_AVAILABLE
	RESET_DOCKLIST(extraBrowserDocks)
#endif
#undef RESET_DOCKLIST

	restoreState(startingDockLayout);
	ui->sideDocks->setChecked(true);

	int cx = width();
	int bottomDocksHeight = height();

	bottomDocksHeight = bottomDocksHeight * 225 / 1000;

	ui->scenesDock->setVisible(true);
	ui->sourcesDock->setVisible(true);
	ui->mixerDock->setVisible(true);
	ui->transitionsDock->setVisible(true);
	controlsDock->setVisible(true);
	statsDock->setVisible(false);
	statsDock->setFloating(true);

	QList<QDockWidget *> bottomDocks{ui->mixerDock, ui->transitionsDock, controlsDock};

	resizeDocks(bottomDocks, {bottomDocksHeight, bottomDocksHeight, bottomDocksHeight}, Qt::Vertical);
	resizeDocks(bottomDocks, {cx * 45 / 100, cx * 14 / 100, cx * 16 / 100}, Qt::Horizontal);

	int sideDockWidth = std::min(width() * 30 / 100, 280);
	resizeDocks({ui->scenesDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);

	if (mediaPlaylistDock)
		mediaPlaylistDock->hide();

	activateWindow();
}

void OBSBasic::on_lockDocks_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	QDockWidget::DockWidgetFeatures mainFeatures = features;
	mainFeatures &= ~QDockWidget::QDockWidget::DockWidgetClosable;

	ui->scenesDock->setFeatures(mainFeatures);
	ui->sourcesDock->setFeatures(mainFeatures);
	ui->mixerDock->setFeatures(mainFeatures);
	ui->transitionsDock->setFeatures(mainFeatures);
	controlsDock->setFeatures(mainFeatures);
	statsDock->setFeatures(features);
	mediaPlaylistDock->setFeatures(features);

	for (int i = extraDocks.size() - 1; i >= 0; i--)
		extraDocks[i]->setFeatures(features);

	for (int i = extraCustomDocks.size() - 1; i >= 0; i--)
		extraCustomDocks[i]->setFeatures(features);

#ifdef BROWSER_AVAILABLE
	for (int i = extraBrowserDocks.size() - 1; i >= 0; i--)
		extraBrowserDocks[i]->setFeatures(features);
#endif
}

void OBSBasic::on_sideDocks_toggled(bool side)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", side);

	setDockCornersVertical(side);
}

void OBSBasic::AddDockWidget(QDockWidget *dock, Qt::DockWidgetArea area, bool extraBrowser)
{
	if (dock->objectName().isEmpty())
		return;

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	setupDockAction(dock);
	dock->setFeatures(features);
	addDockWidget(area, dock);

#ifdef BROWSER_AVAILABLE
	if (extraBrowser && extraBrowserMenuDocksSeparator.isNull())
		extraBrowserMenuDocksSeparator = ui->menuDocks->addSeparator();

	if (!extraBrowser && !extraBrowserMenuDocksSeparator.isNull())
		ui->menuDocks->insertAction(extraBrowserMenuDocksSeparator, dock->toggleViewAction());
	else
		ui->menuDocks->addAction(dock->toggleViewAction());

	if (extraBrowser)
		return;
#else
	UNUSED_PARAMETER(extraBrowser);

	ui->menuDocks->addAction(dock->toggleViewAction());
#endif

	extraDockNames.push_back(dock->objectName());
	extraDocks.push_back(std::shared_ptr<QDockWidget>(dock));
}

void OBSBasic::RemoveDockWidget(const QString &name)
{
	if (extraDockNames.contains(name)) {
		int idx = extraDockNames.indexOf(name);
		extraDockNames.removeAt(idx);
		extraDocks[idx].reset();
		extraDocks.removeAt(idx);
	} else if (extraCustomDockNames.contains(name)) {
		int idx = extraCustomDockNames.indexOf(name);
		extraCustomDockNames.removeAt(idx);
		removeDockWidget(extraCustomDocks[idx]);
		extraCustomDocks.removeAt(idx);
	}
}

bool OBSBasic::IsDockObjectNameUsed(const QString &name)
{
	QStringList list;
	list << "scenesDock"
	     << "sourcesDock"
	     << "mixerDock"
	     << "transitionsDock"
	     << "controlsDock"
	     << "statsDock"
	     << "mediaPlaylistDock";
	list << extraDockNames;
	list << extraCustomDockNames;

	return list.contains(name);
}

void OBSBasic::AddCustomDockWidget(QDockWidget *dock)
{
	// Prevent the object name from being changed
	connect(dock, &QObject::objectNameChanged, this, &OBSBasic::RepairCustomExtraDockName);

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	dock->setFeatures(features);
	addDockWidget(Qt::RightDockWidgetArea, dock);

	extraCustomDockNames.push_back(dock->objectName());
	extraCustomDocks.push_back(dock);
}

void OBSBasic::setDockCornersVertical(bool vertical)
{
	if (vertical) {
		setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	} else {
		setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	}
}

void OBSBasic::RepairCustomExtraDockName()
{
	QDockWidget *dock = reinterpret_cast<QDockWidget *>(sender());
	int idx = extraCustomDocks.indexOf(dock);
	QSignalBlocker block(dock);

	if (idx == -1) {
		blog(LOG_WARNING, "A custom dock got its object name changed");
		return;
	}

	blog(LOG_WARNING, "The custom dock '%s' got its object name restored", QT_TO_UTF8(extraCustomDockNames[idx]));

	dock->setObjectName(extraCustomDockNames[idx]);
}
