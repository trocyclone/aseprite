// Aseprite
// Copyright (C) 2001-2015  David Capello
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/ui/color_bar.h"

#include "app/app.h"
#include "app/app_menus.h"
#include "app/cmd/remap_colors.h"
#include "app/cmd/replace_image.h"
#include "app/cmd/set_palette.h"
#include "app/cmd/set_transparent_color.h"
#include "app/color.h"
#include "app/commands/command.h"
#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/console.h"
#include "app/context_access.h"
#include "app/document_api.h"
#include "app/ini_file.h"
#include "app/modules/gui.h"
#include "app/modules/palettes.h"
#include "app/pref/preferences.h"
#include "app/transaction.h"
#include "app/ui/color_spectrum.h"
#include "app/ui/editor/editor.h"
#include "app/ui/input_chain.h"
#include "app/ui/palette_popup.h"
#include "app/ui/skin/skin_theme.h"
#include "app/ui/status_bar.h"
#include "app/ui/styled_button.h"
#include "app/ui_context.h"
#include "app/ui_context.h"
#include "app/util/clipboard.h"
#include "base/bind.h"
#include "base/scoped_value.h"
#include "doc/cel.h"
#include "doc/cels_range.h"
#include "doc/image.h"
#include "doc/image_impl.h"
#include "doc/palette.h"
#include "doc/primitives.h"
#include "doc/remap.h"
#include "doc/rgbmap.h"
#include "doc/sort_palette.h"
#include "doc/sprite.h"
#include "she/surface.h"
#include "ui/alert.h"
#include "ui/graphics.h"
#include "ui/menu.h"
#include "ui/message.h"
#include "ui/paint_event.h"
#include "ui/separator.h"
#include "ui/splitter.h"
#include "ui/system.h"
#include "ui/tooltips.h"


#include <cstring>


namespace app {

enum class PalButton {
  EDIT,
  SORT,
  PRESETS,
  OPTIONS,
  MAX
};

using namespace app::skin;
using namespace ui;

class ColorBar::WarningIcon : public StyledButton {
public:
  WarningIcon()
    : StyledButton(skin::SkinTheme::instance()->styles.warningBox()) {
  }

protected:
  void onPaint(ui::PaintEvent& ev) override {
    // if (isEnabled())
      StyledButton::onPaint(ev);
    // else
    //   ev.getGraphics()->fillRect(getBgColor(), getClientBounds());
  }
};

//////////////////////////////////////////////////////////////////////
// ColorBar::ScrollableView class

ColorBar::ScrollableView::ScrollableView()
{
  SkinTheme* theme = static_cast<SkinTheme*>(getTheme());
  int l = theme->get_part(PART_EDITOR_SELECTED_W)->width();
  int t = theme->get_part(PART_EDITOR_SELECTED_N)->height();
  int r = theme->get_part(PART_EDITOR_SELECTED_E)->width();
  int b = theme->get_part(PART_EDITOR_SELECTED_S)->height();

  setBorder(gfx::Border(l, t, r, b));
}

void ColorBar::ScrollableView::onPaint(ui::PaintEvent& ev)
{
  ui::Graphics* g = ev.getGraphics();
  SkinTheme* theme = static_cast<SkinTheme*>(getTheme());
  theme->draw_bounds_nw(g,
    getClientBounds(),
    hasFocus() ? PART_EDITOR_SELECTED_NW:
    PART_EDITOR_NORMAL_NW,
    gfx::ColorNone);
}

//////////////////////////////////////////////////////////////////////
// ColorBar class

ColorBar* ColorBar::m_instance = NULL;

ColorBar::ColorBar(int align)
  : Box(align)
  , m_buttons(int(PalButton::MAX))
  , m_paletteView(true, PaletteView::FgBgColors, this,
      Preferences::instance().colorBar.boxSize() * guiscale())
  , m_remapButton("Remap")
  , m_fgColor(app::Color::fromRgb(255, 255, 255), IMAGE_RGB)
  , m_bgColor(app::Color::fromRgb(0, 0, 0), IMAGE_RGB)
  , m_fgWarningIcon(new WarningIcon)
  , m_bgWarningIcon(new WarningIcon)
  , m_lock(false)
  , m_syncingWithPref(false)
  , m_lastDocument(nullptr)
  , m_ascending(true)
{
  m_instance = this;

  SkinTheme* theme = static_cast<SkinTheme*>(getTheme());

  setBorder(gfx::Border(2*guiscale(), 0, 0, 0));
  setChildSpacing(2*guiscale());

  m_paletteView.setColumns(8);
  m_fgColor.setPreferredSize(0, m_fgColor.getPreferredSize().h);
  m_bgColor.setPreferredSize(0, m_bgColor.getPreferredSize().h);

  // TODO hardcoded scroll bar width should be get from skin.xml file
  int scrollBarWidth = 6*guiscale();
  m_scrollableView.getHorizontalBar()->setBarWidth(scrollBarWidth);
  m_scrollableView.getVerticalBar()->setBarWidth(scrollBarWidth);
  setup_mini_look(m_scrollableView.getHorizontalBar());
  setup_mini_look(m_scrollableView.getVerticalBar());

  m_scrollableView.attachToView(&m_paletteView);
  m_scrollableView.setExpansive(true);

  m_remapButton.setVisible(false);

  Box* palViewBox = new VBox();
  palViewBox->addChild(&m_scrollableView);
  palViewBox->addChild(&m_remapButton);

  ColorSpectrum* spectrum = new ColorSpectrum;
  Splitter* splitter = new Splitter(Splitter::ByPercentage, VERTICAL);
  splitter->setPosition(80);
  splitter->setExpansive(true);
  splitter->addChild(palViewBox);
  splitter->addChild(spectrum);

  Box* buttonsBox = new HBox();
  buttonsBox->addChild(&m_buttons);
  m_buttons.setMaxSize(gfx::Size(m_buttons.maxSize().w,
                                 15*ui::guiscale()));

  addChild(buttonsBox);
  addChild(splitter);

  HBox* fgBox = new HBox;
  HBox* bgBox = new HBox;
  fgBox->noBorderNoChildSpacing();
  bgBox->noBorderNoChildSpacing();
  fgBox->addChild(&m_fgColor);
  fgBox->addChild(m_fgWarningIcon);
  bgBox->addChild(&m_bgColor);
  bgBox->addChild(m_bgWarningIcon);
  addChild(fgBox);
  addChild(bgBox);

  m_fgColor.setExpansive(true);
  m_bgColor.setExpansive(true);

  m_remapButton.Click.connect(Bind<void>(&ColorBar::onRemapButtonClick, this));
  m_fgColor.Change.connect(&ColorBar::onFgColorButtonChange, this);
  m_bgColor.Change.connect(&ColorBar::onBgColorButtonChange, this);
  spectrum->ColorChange.connect(&ColorBar::onPickSpectrum, this);
  m_fgWarningIcon->Click.connect(Bind<void>(&ColorBar::onFixWarningClick, this, &m_fgColor, m_fgWarningIcon));
  m_bgWarningIcon->Click.connect(Bind<void>(&ColorBar::onFixWarningClick, this, &m_bgColor, m_bgWarningIcon));

  m_tooltips.addTooltipFor(&m_fgColor, "Foreground color", LEFT);
  m_tooltips.addTooltipFor(&m_bgColor, "Background color", LEFT);
  m_tooltips.addTooltipFor(m_fgWarningIcon, "Add foreground color to the palette", LEFT);
  m_tooltips.addTooltipFor(m_bgWarningIcon, "Add background color to the palette", LEFT);

  // Set background color reading its value from the configuration.
  setBgColor(Preferences::instance().colorBar.bgColor());

  // Clear the selection of the BG color in the palette.
  m_paletteView.deselect();

  // Set foreground color reading its value from the configuration.
  setFgColor(Preferences::instance().colorBar.fgColor());

  // Change color-bar background color (not ColorBar::setBgColor)
  Widget::setBgColor(theme->colors.tabActiveFace());
  m_paletteView.setBgColor(theme->colors.tabActiveFace());

  // Change labels foreground color
  m_buttons.ItemChange.connect(Bind<void>(&ColorBar::onPaletteButtonClick, this));

  m_buttons.addItem(theme->get_part(PART_PAL_EDIT));
  m_buttons.addItem(theme->get_part(PART_PAL_SORT));
  m_buttons.addItem(theme->get_part(PART_PAL_PRESETS));
  m_buttons.addItem(theme->get_part(PART_PAL_OPTIONS));

  // Tooltips
  TooltipManager* tooltipManager = new TooltipManager();
  addChild(tooltipManager);
  tooltipManager->addTooltipFor(m_buttons.getItem((int)PalButton::EDIT), "Edit Color", BOTTOM);
  tooltipManager->addTooltipFor(m_buttons.getItem((int)PalButton::SORT), "Sort & Gradients", BOTTOM);
  tooltipManager->addTooltipFor(m_buttons.getItem((int)PalButton::PRESETS), "Presets", BOTTOM);
  tooltipManager->addTooltipFor(m_buttons.getItem((int)PalButton::OPTIONS), "Options", BOTTOM);
  tooltipManager->addTooltipFor(&m_remapButton, "Matches old indexes with new indexes", BOTTOM);

  onColorButtonChange(getFgColor());

  UIContext::instance()->addObserver(this);
  m_beforeCmdConn = UIContext::instance()->BeforeCommandExecution.connect(&ColorBar::onBeforeExecuteCommand, this);
  m_afterCmdConn = UIContext::instance()->AfterCommandExecution.connect(&ColorBar::onAfterExecuteCommand, this);
  m_fgConn = Preferences::instance().colorBar.fgColor.AfterChange.connect(Bind<void>(&ColorBar::onFgColorChangeFromPreferences, this));
  m_bgConn = Preferences::instance().colorBar.bgColor.AfterChange.connect(Bind<void>(&ColorBar::onBgColorChangeFromPreferences, this));
  m_paletteView.FocusEnter.connect(&ColorBar::onFocusPaletteView, this);
  m_appPalChangeConn = App::instance()->PaletteChange.connect(&ColorBar::onAppPaletteChange, this);
}

ColorBar::~ColorBar()
{
  UIContext::instance()->removeObserver(this);
}

void ColorBar::setPixelFormat(PixelFormat pixelFormat)
{
  m_fgColor.setPixelFormat(pixelFormat);
  m_bgColor.setPixelFormat(pixelFormat);
}

app::Color ColorBar::getFgColor()
{
  return m_fgColor.getColor();
}

app::Color ColorBar::getBgColor()
{
  return m_bgColor.getColor();
}

void ColorBar::setFgColor(const app::Color& color)
{
  m_fgColor.setColor(color);

  if (!m_lock)
    onColorButtonChange(color);
}

void ColorBar::setBgColor(const app::Color& color)
{
  m_bgColor.setColor(color);

  if (!m_lock)
    onColorButtonChange(color);
}

PaletteView* ColorBar::getPaletteView()
{
  return &m_paletteView;
}

void ColorBar::setPaletteEditorButtonState(bool state)
{
  m_buttons.getItem(int(PalButton::EDIT))->setSelected(state);
}

void ColorBar::onActiveSiteChange(const doc::Site& site)
{
  if (m_lastDocument != site.document()) {
    m_lastDocument = site.document();
    hideRemap();
  }
}

void ColorBar::onAppPaletteChange()
{
  fixColorIndex(m_fgColor);
  fixColorIndex(m_bgColor);

  updateWarningIcon(m_fgColor.getColor(), m_fgWarningIcon);
  updateWarningIcon(m_bgColor.getColor(), m_bgWarningIcon);
}

void ColorBar::onFocusPaletteView()
{
  App::instance()->inputChain().prioritize(this);
}

void ColorBar::onBeforeExecuteCommand(Command* command)
{
  if (command->id() == CommandId::SetPalette ||
      command->id() == CommandId::LoadPalette)
    showRemap();
}

void ColorBar::onAfterExecuteCommand(Command* command)
{
  if (command->id() == CommandId::Undo ||
      command->id() == CommandId::Redo)
    invalidate();
}

// Switches the palette-editor
void ColorBar::onPaletteButtonClick()
{
  int item = m_buttons.selectedItem();
  m_buttons.deselectItems();

  switch (static_cast<PalButton>(item)) {

    case PalButton::EDIT: {
      Command* cmd_show_palette_editor = CommandsModule::instance()->getCommandByName(CommandId::PaletteEditor);
      Params params;
      params.set("switch", "true");

      UIContext::instance()->executeCommand(cmd_show_palette_editor, params);
      break;
    }

    case PalButton::SORT: {
      gfx::Rect bounds = m_buttons.getItem(item)->getBounds();

      Menu menu;
      MenuItem
        rev("Reverse Colors"),
        grd("Gradient"),
        hue("Sort by Hue"),
        sat("Sort by Saturation"),
        bri("Sort by Brightness"),
        lum("Sort by Luminance"),
        red("Sort by Red"),
        grn("Sort by Green"),
        blu("Sort by Blue"),
        alp("Sort by Alpha"),
        asc("Ascending"),
        des("Descending");
      menu.addChild(&rev);
      menu.addChild(&grd);
      menu.addChild(new ui::MenuSeparator);
      menu.addChild(&hue);
      menu.addChild(&sat);
      menu.addChild(&bri);
      menu.addChild(&lum);
      menu.addChild(new ui::MenuSeparator);
      menu.addChild(&red);
      menu.addChild(&grn);
      menu.addChild(&blu);
      menu.addChild(&alp);
      menu.addChild(new ui::MenuSeparator);
      menu.addChild(&asc);
      menu.addChild(&des);

      if (m_ascending) asc.setSelected(true);
      else des.setSelected(true);

      rev.Click.connect(Bind<void>(&ColorBar::onReverseColors, this));
      grd.Click.connect(Bind<void>(&ColorBar::onGradient, this));
      hue.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::HUE));
      sat.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::SATURATION));
      bri.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::VALUE));
      lum.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::LUMA));
      red.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::RED));
      grn.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::GREEN));
      blu.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::BLUE));
      alp.Click.connect(Bind<void>(&ColorBar::onSortBy, this, SortPaletteBy::ALPHA));
      asc.Click.connect(Bind<void>(&ColorBar::setAscending, this, true));
      des.Click.connect(Bind<void>(&ColorBar::setAscending, this, false));

      menu.showPopup(gfx::Point(bounds.x, bounds.y+bounds.h));
      break;
    }

    case PalButton::PRESETS: {
      if (!m_palettePopup)
        m_palettePopup.reset(new PalettePopup());

      if (!m_palettePopup->isVisible()) {
        gfx::Rect bounds = m_buttons.getItem(item)->getBounds();

        m_palettePopup->showPopup(
          gfx::Rect(bounds.x, bounds.y+bounds.h,
                    ui::display_w()/2, ui::display_h()/2));
      }
      else {
        m_palettePopup->closeWindow(NULL);
      }
      break;
    }

    case PalButton::OPTIONS: {
      Menu* menu = AppMenus::instance()->getPalettePopupMenu();
      if (menu) {
        gfx::Rect bounds = m_buttons.getItem(item)->getBounds();

        menu->showPopup(gfx::Point(bounds.x, bounds.y+bounds.h));
      }
      break;
    }

  }
}

void ColorBar::onRemapButtonClick()
{
  ASSERT(m_oldPalette);

  // Create remap from m_oldPalette to the current palette
  Remap remap(1);
  try {
    ContextWriter writer(UIContext::instance(), 500);
    Sprite* sprite = writer.sprite();
    ASSERT(sprite);
    if (!sprite)
      return;

    remap = create_remap_to_change_palette(
      m_oldPalette, get_current_palette(), sprite->transparentColor());
  }
  catch (base::Exception& e) {
    Console::showException(e);
  }

  // Check the remap
  if (!remap.isFor8bit() &&
      Alert::show(
        "Automatic Remap"
        "<<The remap operation cannot be perfectly done for more than 256 colors."
        "<<Do you want to continue anyway?"
        "||&OK||&Cancel") != 1) {
    return;
  }

  try {
    ContextWriter writer(UIContext::instance(), 500);
    Sprite* sprite = writer.sprite();
    if (sprite) {
      ASSERT(sprite->pixelFormat() == IMAGE_INDEXED);

      Transaction transaction(writer.context(), "Remap Colors", ModifyDocument);
      bool remapPixels = true;

      if (remap.isFor8bit()) {
        PalettePicks usedEntries(256);

        for (const Cel* cel : sprite->uniqueCels()) {
          for (const auto& i : LockImageBits<IndexedTraits>(cel->image()))
            usedEntries[i] = true;
        }

        if (remap.isInvertible(usedEntries)) {
          transaction.execute(new cmd::RemapColors(sprite, remap));
          remapPixels = false;
        }
      }

      // Special remap saving original images in undo history
      if (remapPixels) {
        for (Cel* cel : sprite->uniqueCels()) {
          ImageRef celImage = cel->imageRef();
          ImageRef newImage(Image::createCopy(celImage.get()));
          doc::remap_image(newImage.get(), remap);

          transaction.execute(new cmd::ReplaceImage(
                                sprite, celImage, newImage));
        }
      }

      color_t oldTransparent = sprite->transparentColor();
      color_t newTransparent = remap[oldTransparent];
      if (oldTransparent != newTransparent)
        transaction.execute(new cmd::SetTransparentColor(sprite, newTransparent));

      transaction.commit();
    }
    update_screen_for_document(writer.document());
    hideRemap();
  }
  catch (base::Exception& e) {
    Console::showException(e);
  }
}

void ColorBar::onPaletteViewIndexChange(int index, ui::MouseButtons buttons)
{
  m_lock = true;

  app::Color color = app::Color::fromIndex(index);

  if ((buttons & kButtonRight) == kButtonRight)
    setBgColor(color);
  else if ((buttons & kButtonLeft) == kButtonLeft)
    setFgColor(color);
  else if ((buttons & kButtonMiddle) == kButtonMiddle)
    setTransparentIndex(index);

  m_lock = false;
}

void ColorBar::onPaletteViewRemapColors(const Remap& remap, const Palette* newPalette)
{
  setPalette(newPalette, "Drag-and-Drop Colors");
}

void ColorBar::setPalette(const doc::Palette* newPalette, const std::string& actionText)
{
  showRemap();

  try {
    ContextWriter writer(UIContext::instance(), 500);
    Sprite* sprite = writer.sprite();
    frame_t frame = writer.frame();
    if (sprite &&
        newPalette->countDiff(sprite->palette(frame), nullptr, nullptr)) {
      Transaction transaction(writer.context(), actionText, ModifyDocument);
      transaction.execute(new cmd::SetPalette(sprite, frame, newPalette));
      transaction.commit();
    }
  }
  catch (base::Exception& e) {
    Console::showException(e);
  }

  set_current_palette(newPalette, false);
  getManager()->invalidate();
}

void ColorBar::setTransparentIndex(int index)
{
  try {
    ContextWriter writer(UIContext::instance(), 500);
    Sprite* sprite = writer.sprite();
    if (sprite &&
        sprite->pixelFormat() == IMAGE_INDEXED &&
        sprite->transparentColor() != index) {
      // TODO merge this code with SpritePropertiesCommand
      Transaction transaction(writer.context(), "Set Transparent Color");
      DocumentApi api = writer.document()->getApi(transaction);
      api.setSpriteTransparentColor(sprite, index);
      transaction.commit();

      update_screen_for_document(writer.document());
    }
  }
  catch (base::Exception& e) {
    Console::showException(e);
  }
}

void ColorBar::onPaletteViewChangeSize(int boxsize)
{
  Preferences::instance().colorBar.boxSize(boxsize);
}

void ColorBar::onPaletteViewPasteColors(
  const Palette* fromPal, const doc::PalettePicks& from, const doc::PalettePicks& _to)
{
  if (!from.picks() || !_to.picks()) // Nothing to do
    return;

  doc::PalettePicks to = _to;
  int to_first = to.firstPick();
  int to_last = to.lastPick();

  // Add extra picks in to range if it's needed to paste more colors.
  int from_picks = from.picks();
  int to_picks = to.picks();
  if (to_picks < from_picks) {
    for (int j=to_last+1; j<to.size() && to_picks<from_picks; ++j) {
      to[j] = true;
      ++to_picks;
    }
  }

  Palette newPalette(*get_current_palette());

  int i = 0;
  int j = to_first;

  for (auto state : from) {
    if (state) {
      if (j < newPalette.size())
        newPalette.setEntry(j, fromPal->getEntry(i));
      else
        newPalette.addEntry(fromPal->getEntry(i));

      for (++j; j<to.size(); ++j)
        if (to[j])
          break;
    }
    ++i;
  }

  setPalette(&newPalette, "Paste Colors");
}

app::Color ColorBar::onPaletteViewGetForegroundIndex()
{
  return getFgColor();
}

app::Color ColorBar::onPaletteViewGetBackgroundIndex()
{
  return getBgColor();
}

void ColorBar::onFgColorChangeFromPreferences()
{
  if (m_syncingWithPref)
    return;

  base::ScopedValue<bool> sync(m_syncingWithPref, true, false);
  setFgColor(Preferences::instance().colorBar.fgColor());
}

void ColorBar::onBgColorChangeFromPreferences()
{
  if (m_syncingWithPref)
    return;

  base::ScopedValue<bool> sync(m_syncingWithPref, true, false);
  setBgColor(Preferences::instance().colorBar.bgColor());
}

void ColorBar::onFgColorButtonChange(const app::Color& color)
{
  if (!m_lock) {
    m_paletteView.deselect();
    m_paletteView.invalidate();
  }

  if (!m_syncingWithPref) {
    base::ScopedValue<bool> sync(m_syncingWithPref, true, false);
    Preferences::instance().colorBar.fgColor(color);
  }

  updateWarningIcon(color, m_fgWarningIcon);
  onColorButtonChange(color);
}

void ColorBar::onBgColorButtonChange(const app::Color& color)
{
  if (!m_lock) {
    m_paletteView.deselect();
    m_paletteView.invalidate();
  }

  if (!m_syncingWithPref) {
    base::ScopedValue<bool> sync(m_syncingWithPref, true, false);
    Preferences::instance().colorBar.bgColor(color);
  }

  updateWarningIcon(color, m_bgWarningIcon);
  onColorButtonChange(color);
}

void ColorBar::onColorButtonChange(const app::Color& color)
{
  if (color.getType() == app::Color::IndexType)
    m_paletteView.selectColor(color.getIndex());
  else {
    m_paletteView.selectExactMatchColor(color);

    // As foreground or background color changed, we've to redraw the
    // palette view fg/bg indicators.
    m_paletteView.invalidate();
  }
}

void ColorBar::onPickSpectrum(const app::Color& color, ui::MouseButtons buttons)
{
  if ((buttons & kButtonRight) == kButtonRight)
    setBgColor(color);
  else
    setFgColor(color);
}

void ColorBar::onReverseColors()
{
  doc::PalettePicks entries;
  m_paletteView.getSelectedEntries(entries);

  entries.pickAllIfNeeded();
  int n = entries.picks();

  std::vector<int> mapToOriginal(n); // Maps index from selectedPalette -> palette
  int i = 0, j = 0;
  for (bool state : entries) {
    if (state)
      mapToOriginal[j++] = i;
    ++i;
  }

  Remap remap(get_current_palette()->size());
  i = 0;
  j = n;
  for (bool state : entries) {
    if (state)
      remap.map(i, mapToOriginal[--j]);
    else
      remap.map(i, i);
    ++i;
  }

  Palette newPalette(*get_current_palette(), remap);
  setPalette(&newPalette, "Reverse Colors");
}

void ColorBar::onSortBy(SortPaletteBy channel)
{
  PalettePicks entries;
  m_paletteView.getSelectedEntries(entries);

  entries.pickAllIfNeeded();
  int n = entries.picks();

  // Create a "subpalette" with selected entries only.
  Palette palette(*get_current_palette());
  Palette selectedPalette(0, n);
  std::vector<int> mapToOriginal(n); // Maps index from selectedPalette -> palette
  int i = 0, j = 0;
  for (bool state : entries) {
    if (state) {
      selectedPalette.setEntry(j, palette.getEntry(i));
      mapToOriginal[j] = i;
      ++j;
    }
    ++i;
  }

  // Create a remap to sort the selected entries with the given color
  // component/channel.
  Remap remap = doc::sort_palette(&selectedPalette, channel, m_ascending);

  // Create a bigger new remap for the original palette (with all
  // entries, selected and deselected).
  Remap remapOrig(palette.size());
  i = j = 0;
  for (bool state : entries) {
    if (state)
      remapOrig.map(i, mapToOriginal[remap[j++]]);
    else
      remapOrig.map(i, i);
    ++i;
  }

  // Create a new palette and apply the remap. This is the final new
  // palette for the sprite.
  Palette newPalette(palette, remapOrig);
  setPalette(&newPalette, "Sort Colors");
}

void ColorBar::onGradient()
{
  int index1, index2;
  if (!m_paletteView.getSelectedRange(index1, index2))
    return;

  Palette newPalette(*get_current_palette());
  newPalette.makeGradient(index1, index2);

  setPalette(&newPalette, "Gradient");
}

void ColorBar::setAscending(bool ascending)
{
  m_ascending = ascending;
}

void ColorBar::showRemap()
{
  doc::Site site = UIContext::instance()->activeSite();
  if (site.sprite() &&
      site.sprite()->pixelFormat() == IMAGE_INDEXED) {
    if (!m_oldPalette) {
      m_oldPalette.reset(new Palette(*get_current_palette()));
      m_remapButton.setVisible(true);
      layout();
    }
  }
}

void ColorBar::hideRemap()
{
  if (!m_oldPalette)
    return;

  m_oldPalette.reset();
  m_remapButton.setVisible(false);
  layout();
}

void ColorBar::onNewInputPriority(InputChainElement* element)
{
  m_paletteView.deselect();
}

bool ColorBar::onCanCut(Context* ctx)
{
  return (m_paletteView.getSelectedEntriesCount() > 0);
}

bool ColorBar::onCanCopy(Context* ctx)
{
  return (m_paletteView.getSelectedEntriesCount() > 0);
}

bool ColorBar::onCanPaste(Context* ctx)
{
  return (clipboard::get_current_format() == clipboard::ClipboardPaletteEntries);
}

bool ColorBar::onCanClear(Context* ctx)
{
  return (m_paletteView.getSelectedEntriesCount() > 0);
}

bool ColorBar::onCut(Context* ctx)
{
  m_paletteView.cutToClipboard();
  return true;
}

bool ColorBar::onCopy(Context* ctx)
{
  m_paletteView.copyToClipboard();
  return true;
}

bool ColorBar::onPaste(Context* ctx)
{
  m_paletteView.pasteFromClipboard();
  return true;
}

bool ColorBar::onClear(Context* ctx)
{
  m_paletteView.clearSelection();
  return true;
}

void ColorBar::onCancel(Context* ctx)
{
  m_paletteView.deselect();
  m_paletteView.discardClipboardSelection();
  invalidate();
}

void ColorBar::onFixWarningClick(ColorButton* colorButton, ui::Button* warningIcon)
{
  try {
    Palette* newPalette = get_current_palette(); // System current pal
    app::Color appColor = colorButton->getColor();
    color_t color = doc::rgba(
      appColor.getRed(),
      appColor.getGreen(),
      appColor.getBlue(),
      appColor.getAlpha());
    int index = newPalette->findExactMatch(
      appColor.getRed(),
      appColor.getGreen(),
      appColor.getBlue(),
      appColor.getAlpha(), -1);

    // It should be -1, because the user has pressed the warning
    // button that is available only when the color isn't in the
    // palette.
    ASSERT(index < 0);
    if (index >= 0)
      return;

    ContextWriter writer(UIContext::instance(), 500);
    Document* document(writer.document());
    Sprite* sprite = writer.sprite();
    if (!document || !sprite) {
      ASSERT(false);
      return;
    }

    newPalette->addEntry(color);
    index = newPalette->size()-1;

    if (document) {
      frame_t frame = writer.frame();

      Transaction transaction(writer.context(), "Add palette entry", ModifyDocument);
      transaction.execute(new cmd::SetPalette(sprite, frame, newPalette));
      transaction.commit();
    }

    set_current_palette(newPalette, true);
    ui::Manager::getDefault()->invalidate();

    warningIcon->setVisible(false);
    warningIcon->getParent()->layout();
  }
  catch (base::Exception& e) {
    Console::showException(e);
  }
}

void ColorBar::updateWarningIcon(const app::Color& color, ui::Button* warningIcon)
{
  int index = get_current_palette()->findExactMatch(
    color.getRed(),
    color.getGreen(),
    color.getBlue(),
    color.getAlpha(), -1);

  warningIcon->setVisible(index < 0);
  warningIcon->getParent()->layout();
}

// static
void ColorBar::fixColorIndex(ColorButton& colorButton)
{
  app::Color color = colorButton.getColor();

  if (color.getType() == Color::IndexType) {
    int oldIndex = color.getIndex();
    int newIndex = MID(0, oldIndex, get_current_palette()->size()-1);
    if (oldIndex != newIndex) {
      color = Color::fromIndex(newIndex);
      colorButton.setColor(color);
    }
  }
}

} // namespace app
