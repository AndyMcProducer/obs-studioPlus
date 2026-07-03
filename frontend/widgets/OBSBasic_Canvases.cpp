/******************************************************************************
    Copyright (C) 2025 by Dennis Sädtler <saedtler@twitch.tv>

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

#include <memory>

namespace {
constexpr const char *vertical_canvas_name = "Vertical Canvas";
}

void OBSBasic::CanvasRemoved(void *data, calldata_t *params)
{
	obs_canvas_t *canvas = static_cast<obs_canvas_t *>(calldata_ptr(params, "canvas"));
	QMetaObject::invokeMethod(static_cast<OBSBasic *>(data), "RemoveCanvas", Q_ARG(OBSCanvas, OBSCanvas(canvas)));
}

OBSCanvas OBSBasic::GetVerticalCanvas() const
{
	if (!verticalCanvasUuid.empty()) {
		OBSCanvas canvas = obs_get_canvas_by_uuid(verticalCanvasUuid.c_str());
		if (canvas)
			return canvas;
	}

	return obs_get_canvas_by_name(vertical_canvas_name);
}

void OBSBasic::EnsureVerticalCanvas()
{
	OBSCanvas canvas = GetVerticalCanvas();
	if (canvas) {
		verticalCanvasUuid = obs_canvas_get_uuid(canvas);
		SyncVerticalCanvasVideoInfo();
		return;
	}

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	const uint32_t baseWidth = ovi.base_width;
	const uint32_t outputWidth = ovi.output_width;
	ovi.base_width = ovi.base_height;
	ovi.base_height = baseWidth;
	ovi.output_width = ovi.output_height;
	ovi.output_height = outputWidth;

	canvas = AddCanvas(vertical_canvas_name, &ovi);
	if (canvas)
		verticalCanvasUuid = obs_canvas_get_uuid(canvas);
}

void OBSBasic::SyncVerticalCanvasVideoInfo()
{
	OBSCanvas canvas = GetVerticalCanvas();
	if (!canvas)
		return;

	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	const uint32_t baseWidth = ovi.base_width;
	const uint32_t outputWidth = ovi.output_width;
	ovi.base_width = ovi.base_height;
	ovi.base_height = baseWidth;
	ovi.output_width = ovi.output_height;
	ovi.output_height = outputWidth;

	obs_canvas_reset_video(canvas, &ovi);
}

const OBS::Canvas &OBSBasic::AddCanvas(const std::string &name, obs_video_info *ovi, int flags)
{
	OBSCanvas canvas = obs_canvas_create(name.c_str(), ovi, flags);
	auto &it = canvases.emplace_back(canvas);
	OnEvent(OBS_FRONTEND_EVENT_CANVAS_ADDED);
	return it;
}

bool OBSBasic::RemoveCanvas(OBSCanvas canvas)
{
	bool removed = false;
	if (!canvas)
		return removed;

	auto canvas_it = std::find(std::begin(canvases), std::end(canvases), canvas);
	if (canvas_it != std::end(canvases)) {
		// Move canvas to a temporary object to delay removal of the canvas and calls to its signal handlers
		// until after erase() completes. This is to avoid issues with recursion coming from the
		// CanvasRemoved() signal handler.
		OBS::Canvas tmp = std::move(*canvas_it);
		canvases.erase(canvas_it);
		removed = true;
	}

	if (removed)
		OnEvent(OBS_FRONTEND_EVENT_CANVAS_REMOVED);

	return removed;
}

void OBSBasic::ClearCanvases()
{
	// Delete canvases one-by-one to ensure OBS_FRONTEND_EVENT_CANVAS_REMOVED is sent for each
	while (!canvases.empty()) {
		RemoveCanvas(OBSCanvas(canvases.back()));
	}
}

void OBSBasic::SwitchEditorCanvas(EditorCanvasType type)
{
	if (activeEditorCanvas == type)
		return;

	if (type == EditorCanvasType::Vertical) {
		EnsureVerticalCanvas();
		if (!GetVerticalCanvas())
			return; // safety: don't switch if canvas creation failed
	}

	activeEditorCanvas = type;

	bool isVertical = (type == EditorCanvasType::Vertical);
	ui->canvasHorizButton->setChecked(!isVertical);
	ui->canvasVertButton->setChecked(isVertical);

	RefreshSceneListForCanvas();
}

void OBSBasic::RefreshSceneListForCanvas()
{
	if (activeEditorCanvas == EditorCanvasType::Vertical) {
		OBSCanvas vc = GetVerticalCanvas();
		if (!vc) {
			ui->scenes->clear();
			ui->sources->Clear();
			currentScene = nullptr;
			return;
		}

		/* Collect vertical-canvas scenes in order (if we have a saved order) */
		struct EnumData {
			QListWidget *lw;
			OBSBasic *main;
		} ed{ui->scenes, this};

		ui->scenes->blockSignals(true);
		sceneListSignals.clear();
		ui->scenes->clear();

		obs_canvas_enum_scenes(vc, [](void *data, obs_source_t *source) {
			EnumData *ed = static_cast<EnumData *>(data);
			obs_scene_t *scene = obs_scene_from_source(source);
			if (!scene)
				return true;

			QListWidgetItem *item = new QListWidgetItem(QT_UTF8(obs_source_get_name(source)));
			SetOBSRef(item, OBSScene(scene));

			signal_handler_t *handler = obs_source_get_signal_handler(source);
			ed->main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "item_add", OBSBasic::SceneItemAdded, ed->main));
			ed->main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "reorder", OBSBasic::SceneReordered, ed->main));
			ed->main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "refresh", OBSBasic::SceneRefreshed, ed->main));

			ed->lw->addItem(item);
			return true;
		}, &ed);

		/* Restore last selected vertical scene */
		if (!verticalCurrentSceneUuid.empty()) {
			OBSSourceAutoRelease src = obs_get_source_by_uuid(verticalCurrentSceneUuid.c_str());
			if (src) {
				for (int i = 0; i < ui->scenes->count(); ++i) {
					auto *item = ui->scenes->item(i);
					OBSScene s = GetOBSRef<OBSScene>(item);
					if (obs_scene_get_source(s) == src.Get()) {
						ui->scenes->setCurrentRow(i);
						break;
					}
				}
			}
		}

		if (ui->scenes->count() > 0 && !ui->scenes->currentItem())
			ui->scenes->setCurrentRow(0);

		ui->scenes->blockSignals(false);

		/* Update source list for the new current vertical scene */
		if (ui->scenes->currentItem()) {
			OBSScene scene = GetOBSRef<OBSScene>(ui->scenes->currentItem());
			currentScene = scene.Get();
			RefreshSources(scene);
		} else {
			currentScene = nullptr;
			ui->sources->Clear();
		}

		/* Resize preview to match vertical canvas dimensions */
		OBSCanvas vc2 = GetVerticalCanvas();
		obs_video_info vci;
		if (obs_canvas_get_video_info(vc2, &vci))
			ResizePreview(vci.base_width, vci.base_height);
	} else {
		/* Restore horizontal (main canvas) scene list */
		ui->scenes->blockSignals(true);
		sceneListSignals.clear();
		ui->scenes->clear();
		obs_enum_scenes([](void *data, obs_source_t *source) {
			OBSBasic *main = static_cast<OBSBasic *>(data);
			OBSCanvasAutoRelease sceneCanvas = obs_source_get_canvas(source);
			if (!sceneCanvas)
				return true;

			OBSCanvas mainCanvas = obs_get_main_canvas();
			if (!mainCanvas)
				return true;

			const char *sceneCanvasUuid = obs_canvas_get_uuid(sceneCanvas);
			const char *mainCanvasUuid = obs_canvas_get_uuid(mainCanvas);
			if (!sceneCanvasUuid || !mainCanvasUuid || strcmp(sceneCanvasUuid, mainCanvasUuid) != 0)
				return true;

			obs_scene_t *scene = obs_scene_from_source(source);
			if (!scene)
				return true;
			QListWidgetItem *item = new QListWidgetItem(QT_UTF8(obs_source_get_name(source)));
			SetOBSRef(item, OBSScene(scene));

			signal_handler_t *handler = obs_source_get_signal_handler(source);
			main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "item_add", OBSBasic::SceneItemAdded, main));
			main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "reorder", OBSBasic::SceneReordered, main));
			main->sceneListSignals.push_back(
				std::make_shared<OBSSignal>(handler, "refresh", OBSBasic::SceneRefreshed, main));

			main->ui->scenes->addItem(item);
			return true;
		}, this);
		ui->scenes->blockSignals(false);

		if (ui->scenes->count() > 0)
			ui->scenes->setCurrentRow(0);
		else {
			currentScene = nullptr;
			ui->sources->Clear();
		}

		ResizePreview(ovi.base_width, ovi.base_height);
	}
}
