#include "overview.hpp"
#include <any>
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/managers/AnimationManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#undef private
#include "OverviewPassElement.hpp"

static void damageMonitor(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview->damage();
}

static void removeOverview(WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) {
    g_pOverview.reset();
}

COverview::~COverview() {
    g_pHyprRenderer->makeEGLCurrent();
    images.clear(); // otherwise we get a vram leak
    g_pInputManager->unsetCursorImage();
    g_pHyprOpenGL->markBlurDirtyForMonitor(pMonitor.lock());
}

COverview::COverview(PHLWORKSPACE startedOn_, bool swipe_) : startedOn(startedOn_), swipe(swipe_) {
    const auto PMONITOR = g_pCompositor->m_lastMonitor.lock();
    pMonitor            = PMONITOR;

    static auto* const* PCOLUMNS = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:columns")->getDataStaticPtr();
    static auto* const* PROWS    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:rows")->getDataStaticPtr();
    static auto* const* PGAPS    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:gap_size")->getDataStaticPtr();
    static auto* const* PCOL     = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:bg_col")->getDataStaticPtr();
    static auto* const* PSKIP    = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:skip_empty")->getDataStaticPtr();

    COLUMNS     = **PCOLUMNS;
    ROWS        = **PROWS;
    GAP_WIDTH   = **PGAPS;
    BG_COLOR    = **PCOL;

    // Always arrange workspaces in a fixed grid starting from workspace 1
    const bool methodCenter  = false;
    const int  methodStartID = 1;

    images.resize(ROWS * COLUMNS);

    // r includes empty workspaces; m skips over them
    std::string selector = **PSKIP ? "m" : "r";

    if (methodCenter) {
        int currentID = methodStartID;
        int firstID   = currentID;

        int backtracked = 0;

        // Initialize tiles to WORKSPACE_INVALID; cliking one of these results
        // in changing to "emptynm" (next empty workspace). Tiles with this id
        // will only remain if skip_empty is on.
        for (size_t i = 0; i < images.size(); i++) {
            images[i].workspaceID = WORKSPACE_INVALID;
        }

        // Scan through workspaces lower than methodStartID until we wrap; count how many
        for (size_t i = 1; i < images.size() / 2; ++i) {
            currentID = getWorkspaceIDNameFromString(selector + "-" + std::to_string(i)).id;
            if (currentID >= firstID)
                break;

            backtracked++;
            firstID = currentID;
        }

        // Scan through workspaces higher than methodStartID. If using "m"
        // (skip_empty), stop when we wrap, leaving the rest of the workspace
        // ID's set to WORKSPACE_INVALID
        for (size_t i = 0; i < (size_t)(ROWS * COLUMNS); ++i) {
            auto& image = images[i];
            if ((int64_t)i - backtracked < 0) {
                currentID = getWorkspaceIDNameFromString(selector + std::to_string((int64_t)i - backtracked)).id;
            } else {
                currentID = getWorkspaceIDNameFromString(selector + "+" + std::to_string((int64_t)i - backtracked)).id;
                if (i > 0 && currentID <= firstID)
                    break;
            }
            image.workspaceID = currentID;
        }

    } else {
        int currentID         = methodStartID;
        images[0].workspaceID = currentID;

        auto PWORKSPACESTART = g_pCompositor->getWorkspaceByID(currentID);
        if (!PWORKSPACESTART)
            PWORKSPACESTART = CWorkspace::create(currentID, pMonitor.lock(), std::to_string(currentID));

        pMonitor->m_activeWorkspace = PWORKSPACESTART;

        // Scan through workspaces higher than methodStartID. If using "m"
        // (skip_empty), stop when we wrap, leaving the rest of the workspace
        // ID's set to WORKSPACE_INVALID
        for (size_t i = 1; i < (size_t)(ROWS * COLUMNS); ++i) {
            auto& image       = images[i];
            currentID         = getWorkspaceIDNameFromString(selector + "+" + std::to_string(i)).id;
            if (currentID <= methodStartID)
                break;
            image.workspaceID = currentID;
        }

        pMonitor->m_activeWorkspace = startedOn;
    }

    g_pHyprRenderer->makeEGLCurrent();

    const auto MON_SIZE = pMonitor->m_size;
    const auto MON_AR   = MON_SIZE.x / MON_SIZE.y;
    const auto GAP      = GAP_WIDTH * pMonitor->m_scale;

    double     maxTileWidthX  = (MON_SIZE.x - (COLUMNS - 1) * GAP) / COLUMNS;
    double     maxTileWidthY  = (MON_SIZE.y - (ROWS - 1) * GAP) * MON_AR / ROWS;

    double     tileWidth  = std::min(maxTileWidthX, maxTileWidthY);
    double     tileHeight = tileWidth / MON_AR;

    m_vTileRenderSize = {tileWidth, tileHeight};

    const double gridWidth  = COLUMNS * m_vTileRenderSize.x + (COLUMNS - 1) * GAP;
    const double gridHeight = ROWS * m_vTileRenderSize.y + (ROWS - 1) * GAP;

    m_vGridOffset = {(MON_SIZE.x - gridWidth) / 2.0, (MON_SIZE.y - gridHeight) / 2.0};

    CBox       monbox{0, 0, m_vTileRenderSize.x * 2, m_vTileRenderSize.y * 2};

    if (!ENABLE_LOWRES)
        monbox = {{0, 0}, pMonitor->m_pixelSize};

    int          currentid = 0;

    PHLWORKSPACE openSpecial = PMONITOR->m_activeSpecialWorkspace;
    if (openSpecial)
        PMONITOR->m_activeSpecialWorkspace.reset();

    g_pHyprRenderer->m_bBlockSurfaceFeedback = true;

    startedOn->m_visible = false;

    for (size_t i = 0; i < (size_t)(ROWS * COLUMNS); ++i) {
        COverview::SWorkspaceImage& image = images[i];
        image.fb.alloc(monbox.w, monbox.h, PMONITOR->m_output->state->state().drmFormat);

        CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
        g_pHyprRenderer->beginRender(PMONITOR, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &image.fb);

        g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

        const auto PWORKSPACE = g_pCompositor->getWorkspaceByID(image.workspaceID);

        if (PWORKSPACE == startedOn)
            currentid = i;

        if (PWORKSPACE) {
            image.pWorkspace          = PWORKSPACE;
            PMONITOR->m_activeWorkspace = PWORKSPACE;
            PWORKSPACE->startAnim(true, true, true);
            PWORKSPACE->m_visible = true;

            if (PWORKSPACE == startedOn)
                PMONITOR->m_activeSpecialWorkspace = openSpecial;

            g_pHyprRenderer->renderWorkspace(PMONITOR, PWORKSPACE, Time::steadyNow(), monbox);

            PWORKSPACE->m_visible = false;
            PWORKSPACE->startAnim(false, false, true);

            if (PWORKSPACE == startedOn)
                PMONITOR->m_activeSpecialWorkspace.reset();
        } else
            g_pHyprRenderer->renderWorkspace(PMONITOR, PWORKSPACE, Time::steadyNow(), monbox);

        image.box = {m_vGridOffset.x + (i % COLUMNS) * (m_vTileRenderSize.x + GAP), m_vGridOffset.y + (i / COLUMNS) * (m_vTileRenderSize.y + GAP), m_vTileRenderSize.x,
                     m_vTileRenderSize.y};

        g_pHyprOpenGL->m_renderData.blockScreenShader = true;
        g_pHyprRenderer->endRender();
    }

    g_pHyprRenderer->m_bBlockSurfaceFeedback = false;

    PMONITOR->m_activeSpecialWorkspace = openSpecial;
    PMONITOR->m_activeWorkspace        = startedOn;
    startedOn->m_visible            = true;
    startedOn->startAnim(true, true, true);

    // zoom on the current workspace.
    // const auto& TILE = images[std::clamp(currentid, 0, ROWS * COLUMNS)];

    g_pAnimationManager->createAnimation(Vector2D{pMonitor->m_size.x * COLUMNS, pMonitor->m_size.y * ROWS}, size, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    g_pAnimationManager->createAnimation((-((pMonitor->m_size / Vector2D{(double)COLUMNS, (double)ROWS}) * Vector2D{currentid % COLUMNS, currentid / COLUMNS}) * pMonitor->m_scale) *
                                             Vector2D{(double)COLUMNS, (double)ROWS}, pos, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);

    size->setUpdateCallback(damageMonitor);
    pos->setUpdateCallback(damageMonitor);

    if (!swipe) {
        *size = pMonitor->m_size;
        *pos  = {0, 0};

        size->setCallbackOnEnd([this](auto) { redrawAll(true); });
    }

    openedID = currentid;

    g_pInputManager->setCursorImageUntilUnset("left_ptr");

    lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;

    auto onCursorMove = [this](void* self, SCallbackInfo& info, std::any param) {
        if (closing)
            return;

        info.cancelled    = true;
        lastMousePosLocal = g_pInputManager->getMouseCoordsInternal() - pMonitor->m_position;
    };

    auto onCursorSelect = [this](void* self, SCallbackInfo& info, std::any param) {
        if (closing)
            return;

        info.cancelled = true;

        // get tile x,y
        int x = lastMousePosLocal.x / pMonitor->m_size.x * COLUMNS;
        int y = lastMousePosLocal.y / pMonitor->m_size.y * ROWS;

        closeOnID = x + y * COLUMNS;

        close();
    };

    mouseMoveHook = g_pHookSystem->hookDynamic("mouseMove", onCursorMove);
    touchMoveHook = g_pHookSystem->hookDynamic("touchMove", onCursorMove);

    mouseButtonHook = g_pHookSystem->hookDynamic("mouseButton", onCursorSelect);
    touchDownHook   = g_pHookSystem->hookDynamic("touchDown", onCursorSelect);
}

void COverview::selectHoveredWorkspace() {
    if (closing)
        return;

    // get tile x,y
    int x = lastMousePosLocal.x / pMonitor->m_size.x * COLUMNS;
    int y = lastMousePosLocal.y / pMonitor->m_size.y * ROWS;
    closeOnID = x + y * COLUMNS;
}

void COverview::redrawID(int id, bool forcelowres) {
    if (pMonitor->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    blockOverviewRendering = true;

    g_pHyprRenderer->makeEGLCurrent();

    id = std::clamp(id, 0, ROWS * COLUMNS);

    CBox monbox{0, 0, m_vTileRenderSize.x * 2, m_vTileRenderSize.y * 2};

    if (!forcelowres && (size->value() != pMonitor->m_size || closing))
        monbox = {{0, 0}, pMonitor->m_pixelSize};

    if (!ENABLE_LOWRES)
        monbox = {{0, 0}, pMonitor->m_pixelSize};

    auto& image = images[id];

    if (image.fb.m_size != monbox.size()) {
        image.fb.release();
        image.fb.alloc(monbox.w, monbox.h, pMonitor->m_output->state->state().drmFormat);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(pMonitor.lock(), fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &image.fb);

    g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

    const auto   PWORKSPACE = image.pWorkspace;

    PHLWORKSPACE openSpecial = pMonitor->m_activeSpecialWorkspace;
    if (openSpecial)
        pMonitor->m_activeSpecialWorkspace.reset();

    startedOn->m_visible = false;

    if (PWORKSPACE) {
        pMonitor->m_activeWorkspace = PWORKSPACE;
        PWORKSPACE->startAnim(true, true, true);
        PWORKSPACE->m_visible = true;

        if (PWORKSPACE == startedOn)
            pMonitor->m_activeSpecialWorkspace = openSpecial;

        g_pHyprRenderer->renderWorkspace(pMonitor.lock(), PWORKSPACE, Time::steadyNow(), monbox);

        PWORKSPACE->m_visible = false;
        PWORKSPACE->startAnim(false, false, true);

        if (PWORKSPACE == startedOn)
            pMonitor->m_activeSpecialWorkspace.reset();
    } else
        g_pHyprRenderer->renderWorkspace(pMonitor.lock(), PWORKSPACE, Time::steadyNow(), monbox);

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    pMonitor->m_activeSpecialWorkspace = openSpecial;
    pMonitor->m_activeWorkspace        = startedOn;
    startedOn->m_visible            = true;
    startedOn->startAnim(true, true, true);

    blockOverviewRendering = false;
}

void COverview::redrawAll(bool forcelowres) {
    for (size_t i = 0; i < (size_t)(ROWS * COLUMNS); ++i) {
        redrawID(i, forcelowres);
    }
}

void COverview::damage() {
    blockDamageReporting = true;
    g_pHyprRenderer->damageMonitor(pMonitor.lock());
    blockDamageReporting = false;
}

void COverview::onDamageReported() {
    damageDirty = true;

    Vector2D    SIZE = size->value();

    // onDamageReported is called during zoom animation. The logic in fullRender is sufficient
    // for redrawing, so we can simplify this to just damage the whole monitor.
    // CBox texbox = ...
    // g_pHyprRenderer->damageBox(texbox);

    damage();

    blockDamageReporting = true;
    g_pHyprRenderer->damageBox(CBox{0, 0, pMonitor->m_size.x, pMonitor->m_size.y});
    blockDamageReporting = false;
    g_pCompositor->scheduleFrameForMonitor(pMonitor.lock());
}

void COverview::close() {
    if (closing)
        return;

    const int   ID = closeOnID == -1 ? openedID : closeOnID;

    const auto& TILE = images[std::clamp(ID, 0, ROWS * COLUMNS)];

    Vector2D    tileSize = {pMonitor->m_size.x / COLUMNS, pMonitor->m_size.y / ROWS};

    *size = Vector2D{pMonitor->m_size.x * COLUMNS, pMonitor->m_size.y * ROWS};
    *pos  = (-((pMonitor->m_size / Vector2D{(double)COLUMNS, (double)ROWS}) * Vector2D{ID % COLUMNS, ID / COLUMNS}) * pMonitor->m_scale) * Vector2D{(double)COLUMNS, (double)ROWS};

    size->setCallbackOnEnd(removeOverview);

    closing = true;

    redrawAll();

    if (TILE.workspaceID != pMonitor->activeWorkspaceID()) {
        pMonitor->setSpecialWorkspace(0);

        // If this tile's workspace was WORKSPACE_INVALID, move to the next
        // empty workspace. This should only happen if skip_empty is on, in
        // which case some tiles will be left with this ID intentionally.
        const int  NEWID = TILE.workspaceID == WORKSPACE_INVALID ? getWorkspaceIDNameFromString("emptynm").id : TILE.workspaceID;

        const auto NEWIDWS = g_pCompositor->getWorkspaceByID(NEWID);

        const auto OLDWS = pMonitor->m_activeWorkspace;

        if (!NEWIDWS)
            g_pKeybindManager->changeworkspace(std::to_string(NEWID));
        else
            g_pKeybindManager->changeworkspace(NEWIDWS->getConfigName());

        pMonitor->m_activeWorkspace->startAnim(true, true, true);
        OLDWS->startAnim(false, false, true);

        startedOn = pMonitor->m_activeWorkspace;
    }
}

void COverview::onPreRender() {
    if (damageDirty) {
        damageDirty = false;
        redrawID(closing ? (closeOnID == -1 ? openedID : closeOnID) : openedID);
    }
}

void COverview::onWorkspaceChange() {
    if (valid(startedOn))
        startedOn->startAnim(false, false, true);
    else
        startedOn = pMonitor->m_activeWorkspace;

    for (size_t i = 0; i < (size_t)(ROWS * COLUMNS); ++i) {
        if (images[i].workspaceID != pMonitor->activeWorkspaceID())
            continue;

        openedID = i;
        break;
    }

    closeOnID = openedID;
    close();
}

void COverview::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<COverviewPassElement>());
}

void COverview::fullRender() {
    const auto GAPSIZE = (closing ? (1.0 - size->getPercent()) : size->getPercent()) * GAP_WIDTH;

    if (pMonitor->m_activeWorkspace != startedOn && !closing) {
        // likely user changed.
        onWorkspaceChange();
    }

    const Vector2D SIZE   = size->value();
    const auto     MON_AR = pMonitor->m_size.x / pMonitor->m_size.y;

    // Recalculate tile size and offset based on current animated SIZE
    double         maxTileWidthX  = (SIZE.x - (COLUMNS - 1) * GAPSIZE) / COLUMNS;
    double         maxTileWidthY  = (SIZE.y - (ROWS - 1) * GAPSIZE) * MON_AR / ROWS;

    double         tileWidth  = std::min(maxTileWidthX, maxTileWidthY);
    double         tileHeight = tileWidth / MON_AR;

    const Vector2D tileRenderSize = {tileWidth, tileHeight};

    const double   gridWidth  = COLUMNS * tileRenderSize.x + (COLUMNS - 1) * GAPSIZE;
    const double   gridHeight = ROWS * tileRenderSize.y + (ROWS - 1) * GAPSIZE;

    const Vector2D gridOffset = {(SIZE.x - gridWidth) / 2.0, (SIZE.y - gridHeight) / 2.0};

    g_pHyprOpenGL->clear(BG_COLOR.stripA());

    for (size_t y = 0; y < (size_t)ROWS; ++y) {
        for (size_t x = 0; x < (size_t)COLUMNS; ++x) {
            CBox texbox = {gridOffset.x + x * (tileRenderSize.x + GAPSIZE), gridOffset.y + y * (tileRenderSize.y + GAPSIZE), tileRenderSize.x, tileRenderSize.y};
            texbox.scale(pMonitor->m_scale).translate(pos->value());
            texbox.round();
            CRegion damage{0, 0, INT16_MAX, INT16_MAX};
            g_pHyprOpenGL->renderTextureInternalWithDamage(images[x + y * COLUMNS].fb.getTexture(), texbox, 1.0, damage);
        }
    }
}

static float lerp(const float& from, const float& to, const float perc) {
    return (to - from) * perc + from;
}

static Vector2D lerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return Vector2D{lerp(from.x, to.x, perc), lerp(from.y, to.y, perc)};
}

void COverview::onSwipeUpdate(double delta) {
    if (swipeWasCommenced)
        return;

    static auto* const* PDISTANCE = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprexpo:gesture_distance")->getDataStaticPtr();

    const float         PERC = 1.0 - std::clamp(delta / (double)**PDISTANCE, 0.0, 1.0);

    const auto SIZEMAX = Vector2D{pMonitor->m_size.x * COLUMNS, pMonitor->m_size.y * ROWS};
    const auto POSMAX  = (-((pMonitor->m_size / Vector2D{(double)COLUMNS, (double)ROWS}) * Vector2D{openedID % COLUMNS, openedID / COLUMNS}) * pMonitor->m_scale) *
                      Vector2D{(double)COLUMNS, (double)ROWS};

    const auto SIZEMIN = pMonitor->m_size;
    const auto POSMIN  = Vector2D{0, 0};

    size->setValueAndWarp(lerp(SIZEMIN, SIZEMAX, PERC));
    pos->setValueAndWarp(lerp(POSMIN, POSMAX, PERC));
}

void COverview::onSwipeEnd() {
    const auto SIZEMIN = pMonitor->m_size;
    const auto SIZEMAX = Vector2D{pMonitor->m_size.x * COLUMNS, pMonitor->m_size.y * ROWS};
    const auto PERC    = (size->value() - SIZEMIN).x / (SIZEMAX - SIZEMIN).x;
    if (PERC > 0.5) {
        close();
        return;
    }
    *size = pMonitor->m_size;
    *pos  = {0, 0};

    size->setCallbackOnEnd([this](WP<Hyprutils::Animation::CBaseAnimatedVariable> thisptr) { redrawAll(true); });

    swipeWasCommenced = true;
}
