/*
 * Copyright (C) 2012 Emweb bvba, Kessel-Lo, Belgium.
 *
 * See the LICENSE file for terms of use.
 */

#ifndef OLD_LAYOUT

#include <boost/lexical_cast.hpp>
#include <sstream>

#include "Wt/WApplication"
#include "Wt/WContainerWidget"
#include "Wt/WEnvironment"
#include "Wt/WGridLayout"
#include "Wt/WLogger"

#include "StdGridLayoutImpl2.h"
#include "SizeHandle.h"
#include "DomElement.h"
#include "WebUtils.h"

#ifndef WT_DEBUG_JS
#include "js/StdGridLayoutImpl2.min.js"
#include "js/WtResize.min.js"
#endif

#ifdef WIN32
#define snprintf _snprintf
#endif

namespace Wt {

LOGGER("WGridLayout2");

StdGridLayoutImpl2::StdGridLayoutImpl2(WLayout *layout, Impl::Grid& grid)
  : StdLayoutImpl(layout),
    grid_(grid),
    needAdjust_(false),
    needConfigUpdate_(false)
{
  const char *THIS_JS = "js/StdGridLayoutImpl2.js";

  WApplication *app = WApplication::instance();

  if (!app->javaScriptLoaded(THIS_JS)) {
    app->styleSheet().addRule("table.Wt-hcenter", "margin: 0px auto;"
			      "position: relative");

    LOAD_JAVASCRIPT(app, THIS_JS, "StdLayout2", wtjs1);
    LOAD_JAVASCRIPT(app, THIS_JS, "layouts2", appjs1);

    app->doJavaScript(app->javaScriptClass() + ".layouts2.scheduleAdjust();");
    app->doJavaScript("$(window).load(function() { "
		      + app->javaScriptClass() + ".layouts2.scheduleAdjust();"
		      + "});");

    WApplication::instance()->addAutoJavaScript
      (app->javaScriptClass() + ".layouts2.adjustNow();");
  }
}

bool StdGridLayoutImpl2::itemResized(WLayoutItem *item)
{
  const unsigned colCount = grid_.columns_.size();
  const unsigned rowCount = grid_.rows_.size();

  for (unsigned row = 0; row < rowCount; ++row)
    for (unsigned col = 0; col < colCount; ++col)
      if (grid_.items_[row][col].item_ == item &&
	  !grid_.items_[row][col].update_) {
	grid_.items_[row][col].update_ = true;
	needAdjust_ = true;
	return true;
      }

  return false;
}

int StdGridLayoutImpl2::nextRowWithItem(int row, int c) const
{
  for (row += grid_.items_[row][c].rowSpan_; row < (int)grid_.rows_.size();
       ++row) {
    for (unsigned col = 0; col < grid_.columns_.size();
	 col += grid_.items_[row][col].colSpan_) {
      if (hasItem(row, col))
	return row;
    }
  }

  return grid_.rows_.size();
}

int StdGridLayoutImpl2::nextColumnWithItem(int row, int col) const
{
  for (;;) {
    col = col + grid_.items_[row][col].colSpan_;

    if (col < (int)grid_.columns_.size()) {
      for (unsigned i = 0; i < grid_.rows_.size(); ++i)
	if (hasItem(i, col))
	  return col;
    } else
      return grid_.columns_.size();
  }
}

bool StdGridLayoutImpl2::hasItem(int row, int col) const
{
  WLayoutItem *item = grid_.items_[row][col].item_;

  if (item) {
    WWidget *w = item->widget();
    return !w || !w->isHidden();
  } else
    return false;
}

DomElement *StdGridLayoutImpl2::createElement(WLayoutItem *item,
					      WApplication *app)
{
  DomElement *c = getImpl(item)->createDomElement(true, true, app); 

  c->setProperty(PropertyStyleVisibility, "hidden");

  return c;
}

void StdGridLayoutImpl2::updateDom(DomElement& parent)
{
  WApplication *app = WApplication::instance();

  if (needConfigUpdate_) {
    needConfigUpdate_ = false;

    DomElement *div = DomElement::getForUpdate(this, DomElement_DIV);

    for (unsigned i = 0; i < addedItems_.size(); ++i) {
      WLayoutItem *item = addedItems_[i];
      DomElement *c = createElement(item, app);
      div->addChild(c);
    }

    addedItems_.clear();

    for (unsigned i = 0; i < removedItems_.size(); ++i)
      parent.callJavaScript(WT_CLASS ".remove('" + removedItems_[i] + "');",
			    true);

    removedItems_.clear();

    parent.addChild(div);

    WStringStream js;
    js << app->javaScriptClass() << ".layouts2.updateConfig('"
       << id() << "',";
    streamConfig(js, app);
    js << ");";

    app->doJavaScript(js.str());
  }

  if (needAdjust_) {
    needAdjust_ = false;

    WStringStream js;
    js << app->javaScriptClass() << ".layouts2.adjust('" << id() << "', [";

    bool first = true;

    const unsigned colCount = grid_.columns_.size();
    const unsigned rowCount = grid_.rows_.size();

    for (unsigned row = 0; row < rowCount; ++row)
      for (unsigned col = 0; col < colCount; ++col)
	if (grid_.items_[row][col].update_) {
	  grid_.items_[row][col].update_ = false;
	  if (!first)
	    js << ",";
	  first = false;
	  js << "[" << (int)row << "," << (int)col << "]";
	}

    js << "]);";

    app->doJavaScript(js.str());
  }

  const unsigned colCount = grid_.columns_.size();
  const unsigned rowCount = grid_.rows_.size();

  for (unsigned i = 0; i < rowCount; ++i) {
    for (unsigned j = 0; j < colCount; ++j) {
      WLayoutItem *item = grid_.items_[i][j].item_;
      if (item) {
	WLayout *nested = item->layout();
	if (nested)
	  (dynamic_cast<StdLayoutImpl *>(nested->impl()))->updateDom(parent);
      }
    }
  }
}

StdGridLayoutImpl2::~StdGridLayoutImpl2()
{ 
  WApplication *app = WApplication::instance();

  /*
   * If it is a top-level layout (as opposed to a nested layout),
   * configure overflow of the container.
   */
  if (parentLayoutImpl() == 0) {
    if (container() == app->root()) {
      app->setBodyClass("");
      app->setHtmlClass("");
    }

    if (app->environment().agentIsIElt(9) && container())
      container()->setOverflow(WContainerWidget::OverflowVisible);
  }
}

int StdGridLayoutImpl2::minimumHeightForRow(int row) const
{
  int minHeight = 0;

  const unsigned colCount = grid_.columns_.size();
  for (unsigned j = 0; j < colCount; ++j) {
    WLayoutItem *item = grid_.items_[row][j].item_;
    if (item)
      minHeight = std::max(minHeight, getImpl(item)->minimumHeight());
  }

  return minHeight;
}

int StdGridLayoutImpl2::minimumWidthForColumn(int col) const
{
  int minWidth = 0;

  const unsigned rowCount = grid_.rows_.size();
  for (unsigned i = 0; i < rowCount; ++i) {
    WLayoutItem *item = grid_.items_[i][col].item_;
    if (item)
      minWidth = std::max(minWidth, getImpl(item)->minimumWidth());
  }

  return minWidth;
}

int StdGridLayoutImpl2::minimumWidth() const
{
  const unsigned colCount = grid_.columns_.size();

  int total = 0;

  for (unsigned i = 0; i < colCount; ++i)
    total += minimumWidthForColumn(i);

  return total + (colCount-1) * grid_.horizontalSpacing_;
}

int StdGridLayoutImpl2::minimumHeight() const
{
  const unsigned rowCount = grid_.rows_.size();

  int total = 0;

  for (unsigned i = 0; i < rowCount; ++i)
    total += minimumHeightForRow(i);

  return total + (rowCount-1) * grid_.verticalSpacing_;
}

void StdGridLayoutImpl2::updateAddItem(WLayoutItem *item)
{
  StdLayoutImpl::updateAddItem(item);

  addedItems_.push_back(item);
}

void StdGridLayoutImpl2::updateRemoveItem(WLayoutItem *item)
{
  StdLayoutImpl::updateRemoveItem(item);

  Utils::erase(addedItems_, item);
  removedItems_.push_back(getImpl(item)->id());
}

void StdGridLayoutImpl2::update(WLayoutItem *item)
{
  WContainerWidget *c = container();

  if (c)
    c->layoutChanged(false, false);

  needConfigUpdate_ = true;
}

void StdGridLayoutImpl2::containerAddWidgets(WContainerWidget *container)
{
  StdLayoutImpl::containerAddWidgets(container);

  if (!container)
    return;

  WApplication *app = WApplication::instance();

  /*
   * If it is a top-level layout (as opposed to a nested layout),
   * configure overflow of the container.
   */
  if (parentLayoutImpl() == 0) {
    if (container == app->root()) {
      /*
       * Reset body,html default paddings and so on if we are doing layout
       * in the entire document.
       */
      app->setBodyClass(app->bodyClass() + " Wt-layout");
      app->setHtmlClass(app->htmlClass() + " Wt-layout");
    }
  }
}

void StdGridLayoutImpl2::setHint(const std::string& name,
				 const std::string& value)
{
  LOG_ERROR("unrecognized hint '" << name << "'");
}

void StdGridLayoutImpl2
::streamConfig(WStringStream& js,
	       const std::vector<Impl::Grid::Section>& sections,
	       bool rows, WApplication *app)
{
  js << "[";

  for (unsigned i = 0; i < sections.size(); ++i) {
    if (i != 0)
      js << ",";

    js << "[" << sections[i].stretch_ << ",";

    if (sections[i].resizable_) {
      SizeHandle::loadJavaScript(app);

      js << "[";

      const WLength& size = sections[i].initialSize_;

      if (size.isAuto())
	js << "-1";
      else if (size.unit() == WLength::Percentage)
	js << size.value() << ",1";
      else
	js << size.toPixels();

      js << "],";
    } else
      js << "0,";

    if (rows)
      js << minimumHeightForRow(i);
    else
      js << minimumWidthForColumn(i);

    js << "]";

  }

  js << "]";
}

void StdGridLayoutImpl2::streamConfig(WStringStream& js, WApplication *app)
{
  js << "{ rows:";

  streamConfig(js, grid_.rows_, true, app);

  js << ", cols:";

  streamConfig(js, grid_.columns_, false, app);

  js << ", items: [";

  const unsigned colCount = grid_.columns_.size();
  const unsigned rowCount = grid_.rows_.size();

  for (unsigned row = 0; row < rowCount; ++row) {
    for (unsigned col = 0; col < colCount; ++col) {
      Impl::Grid::Item& item = grid_.items_[row][col];

      AlignmentFlag hAlign = item.alignment_ & AlignHorizontalMask;
      AlignmentFlag vAlign = item.alignment_ & AlignVerticalMask;

      if (row + col != 0)
	js << ",";

      if (item.item_) {
	std::string id = getImpl(item.item_)->id();

	js << "{";

	if (item.colSpan_ != 1 || item.rowSpan_ != 1)
	  js << "span: [" << item.colSpan_ << "," << item.rowSpan_ << "],";

	if (item.alignment_) {
	  unsigned align = 0;

	  if (hAlign != 0) switch (hAlign) {
	    case AlignLeft: align |= 0x1; break;
	    case AlignRight: align |= 0x2; break;
	    case AlignCenter: align |= 0x4; break;
	    default: break;
	    }

	  if (vAlign != 0) switch (vAlign) {
	    case AlignTop: align |= 0x10; break;
	    case AlignBottom: align |= 0x20; break;
	    case AlignMiddle: align |= 0x40; break;
	    default: break;
	    }

	  js << "align:" << (int)align << ",";
	}

	js << "dirty:2,id:'" << id << "'"
	   << "}";
      } else
	js << "null";
    }
  }

  js << "]}";
}

int StdGridLayoutImpl2::pixelSize(const WLength& size)
{
  if (size.unit() == WLength::Percentage)
    return 0;
  else
    return (int)size.toPixels();
}

/*
 * fitWidth, fitHeight:
 *  - from setLayout(AlignLeft | AlignTop)
 *    is being deprecated but still needs to be implemented
 *  - nested layouts: handles as other layout items
 */
DomElement *StdGridLayoutImpl2::createDomElement(bool fitWidth, bool fitHeight,
						 WApplication *app)
{
  needAdjust_ = needConfigUpdate_ = false;
  addedItems_.clear();
  removedItems_.clear();

  const unsigned colCount = grid_.columns_.size();
  const unsigned rowCount = grid_.rows_.size();

  int margin[] = { 0, 0, 0, 0};

  int maxWidth = 0, maxHeight = 0;

  if (layout()->parentLayout() == 0) {
#ifndef WT_TARGET_JAVA
    layout()->getContentsMargins(margin + 3, margin, margin + 1, margin + 2);
#else // WT_TARGET_JAVA
    margin[3] = layout()->getContentsMargin(Left);
    margin[0] = layout()->getContentsMargin(Top);
    margin[1] = layout()->getContentsMargin(Right);
    margin[2] = layout()->getContentsMargin(Bottom);
#endif // WT_TARGET_JAVA

    maxWidth = pixelSize(container()->maximumWidth());
    maxHeight = pixelSize(container()->maximumHeight());
  }

  WStringStream js;

  js << app->javaScriptClass()
     << ".layouts2.add(new " WT_CLASS ".StdLayout2("
     << app->javaScriptClass() << ",'"
     << id() << "',";

  if (layout()->parentLayout())
    js << "'" << getImpl(layout()->parentLayout())->id() << "',";
  else
    js << "null,";

  bool progressive = !app->environment().ajax();
  js << (fitWidth ? '1' : '0') << "," << (fitHeight ? '1' : '0') << ","
     << (progressive ? '1' : '0') << ",";

  js << maxWidth << "," << maxHeight
     << ",["
     << grid_.horizontalSpacing_ << "," << margin[3] << "," << margin[1]
     << "],["
     << grid_.verticalSpacing_ << "," << margin[0] << "," << margin[2] << "],";

  streamConfig(js, app);

  DomElement *div = DomElement::createNew(DomElement_DIV);
  div->setId(id());
  div->setProperty(PropertyStylePosition, "relative");

  DomElement *table = 0, *tbody = 0, *tr = 0;
  if (progressive) {
    table = DomElement::createNew(DomElement_TABLE);

    WStringStream style;
    if (maxWidth)
      style << "max-width: " << maxWidth << "px;";
    if (maxHeight)
      style << "max-height: " << maxHeight << "px;";
    style << "width: 100%;";

    table->setProperty(PropertyStyle, style.str());

    int totalColStretch = 0;
    for (unsigned col = 0; col < colCount; ++col)
      totalColStretch += std::max(0, grid_.columns_[col].stretch_);

    for (unsigned col = 0; col < colCount; ++col) {
      DomElement *c = DomElement::createNew(DomElement_COL);
      int stretch = std::max(0, grid_.columns_[col].stretch_);

      if (stretch || totalColStretch == 0) {
	char buf[30];

	double pct = totalColStretch == 0 ? 100.0 / colCount
	  : (100.0 * stretch / totalColStretch);

	WStringStream ss;
	ss << "width:" << Utils::round_css_str(pct, 2, buf) << "%;";
	c->setProperty(PropertyStyle, ss.str());
      }

      table->addChild(c);
    }

    tbody = DomElement::createNew(DomElement_TBODY);
  }

#ifndef WT_TARGET_JAVA
  std::vector<bool> overSpanned(colCount * rowCount, false);
#else
  std::vector<bool> overSpanned;
  overSpanned.insert(0, colCount * rowCount, false);
#endif // WT_TARGET_JAVA

  int prevRowWithItem = -1;

  for (unsigned row = 0; row < rowCount; ++row) {
    if (table)
      tr = DomElement::createNew(DomElement_TR);

    bool rowVisible = false;
    int prevColumnWithItem = -1;

    for (unsigned col = 0; col < colCount; ++col) {
      Impl::Grid::Item& item = grid_.items_[row][col];

      if (!overSpanned[row * colCount + col]) {
	for (int i = 0; i < item.rowSpan_; ++i)
	  for (int j = 0; j < item.colSpan_; ++j)
	    if (i + j > 0)
	      overSpanned[(row + i) * colCount + col + j] = true;

	AlignmentFlag hAlign = item.alignment_ & AlignHorizontalMask;
	AlignmentFlag vAlign = item.alignment_ & AlignVerticalMask;

	DomElement *td = 0;

	if (table) {
	  bool itemVisible = hasItem(row, col);
	  rowVisible = rowVisible || itemVisible;

	  td = DomElement::createNew(DomElement_TD);

	  if (itemVisible) {
	    int padding[] = { 0, 0, 0, 0 };

	    int nextRow = nextRowWithItem(row, col);
	    int prevRow = prevRowWithItem;

	    int nextCol = nextColumnWithItem(row, col);
	    int prevCol = prevColumnWithItem;

	    if (prevRow == -1)
	      padding[0] = margin[0];
	    else
	      padding[0] = (grid_.verticalSpacing_+1) / 2;

	    if (nextRow == (int)rowCount)
	      padding[2] = margin[2];
	    else
	      padding[2] = grid_.verticalSpacing_ / 2;

	    if (prevCol == -1)
	      padding[3] = margin[3];
	    else
	      padding[3] = (grid_.horizontalSpacing_ + 1)/2;

	    if (nextCol == (int)colCount)
	      padding[1] = margin[1];
	    else
	      padding[1] = (grid_.horizontalSpacing_)/2;

	    WStringStream style;

	    if (app->layoutDirection() == RightToLeft)
	      std::swap(padding[1], padding[3]);

	    if (padding[0] == padding[1] && padding[0] == padding[2]
		&& padding[0] == padding[3]) {
	      if (padding[0] != 0)
		style << "padding:" << padding[0] << "px;";
	    } else
	      style << "padding:"
		    << padding[0] << "px " << padding[1] << "px "
		    << padding[2] << "px " << padding[3] << "px;";

	    if (vAlign != 0) switch (vAlign) {
	      case AlignTop:
		style << "vertical-align:top;";
		break;
	      case AlignMiddle:
		style << "vertical-align:middle;";
		break;
	      case AlignBottom:
		style << "vertical-align:bottom;";
	      default:
		break;
	      }

	    td->setProperty(PropertyStyle, style.str());

	    if (item.rowSpan_ != 1)
	      td->setProperty(PropertyRowSpan,
			      boost::lexical_cast<std::string>(item.rowSpan_));
	    if (item.colSpan_ != 1)
	      td->setProperty(PropertyColSpan,
			      boost::lexical_cast<std::string>(item.colSpan_));

	    prevColumnWithItem = col;
	  }
	}

	DomElement *c = 0;

	if (!table) {
	  if (item.item_) {
	    c = createElement(item.item_, app);
	    div->addChild(c);
	  }
	} else
	  if (item.item_)
	    c = getImpl(item.item_)->createDomElement(true, true, app);

	if (table) {
	  if (c) {
	    if (!app->environment().agentIsIE())
	      c->setProperty(PropertyStyleBoxSizing, "border-box");

	    if (hAlign == 0)
	      hAlign = AlignJustify;

	    switch (hAlign) {
	    case AlignCenter: {
	      DomElement *itable = DomElement::createNew(DomElement_TABLE);
	      itable->setProperty(PropertyClass, "Wt-hcenter");
	      if (vAlign == 0)
		itable->setProperty(PropertyStyle, "height:100%;");
	      DomElement *irow = DomElement::createNew(DomElement_TR);
	      DomElement *itd = DomElement::createNew(DomElement_TD);
	      if (vAlign == 0)
		itd->setProperty(PropertyStyle, "height:100%;");
	      itd->addChild(c);

	      if (app->environment().agentIsIElt(9)) {
		// IE7 and IE8 do support min-width but do not enforce it
		// properly when in a table.
		//  see http://stackoverflow.com/questions/2356525
		//            /css-min-width-in-ie6-7-and-8
		if (!c->getProperty(PropertyStyleMinWidth).empty()) {
		  DomElement *spacer = DomElement::createNew(DomElement_DIV);
		  spacer->setProperty(PropertyStyleWidth,
				      c->getProperty(PropertyStyleMinWidth));
		  spacer->setProperty(PropertyStyleHeight, "1px");
		  itd->addChild(spacer);
		}
	      }

	      irow->addChild(itd);
	      itable->addChild(irow);
	      c = itable;
	      break;
	    }
	    case AlignRight:
	      if (!c->isDefaultInline())
		c->setProperty(PropertyStyleFloat, "right");
	      else
		td->setProperty(PropertyStyleTextAlign, "right");
	      break;
	    case AlignLeft:
	      if (!c->isDefaultInline())
		c->setProperty(PropertyStyleFloat, "left");
	      else
		td->setProperty(PropertyStyleTextAlign, "left");
	      break;
	    default:
	      break;
	    }

	    td->addChild(c);

	    if (app->environment().agentIsIElt(9)) {
	      // IE7 and IE8 do support min-width but do not enforce it properly
	      // when in a table.
	      //  see http://stackoverflow.com/questions/2356525
	      //            /css-min-width-in-ie6-7-and-8
	      if (!c->getProperty(PropertyStyleMinWidth).empty()) {
		DomElement *spacer = DomElement::createNew(DomElement_DIV);
		spacer->setProperty(PropertyStyleWidth,
				    c->getProperty(PropertyStyleMinWidth));
		spacer->setProperty(PropertyStyleHeight, "1px");
		td->addChild(spacer);
	      }
	    }
	  }

	  tr->addChild(td);
	}
      }
    }

    if (tr) {
      if (!rowVisible)
	tr->setProperty(PropertyStyleDisplay, "hidden");
      else
	prevRowWithItem = row;
      tbody->addChild(tr);
    }
  }

  js << "));";

  if (table) {
    table->addChild(tbody);
    div->addChild(table);
  }

  div->callJavaScript(js.str());

  return div;
}

}

#endif // OLD_LAYOUT
