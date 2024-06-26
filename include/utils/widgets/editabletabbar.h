/*
 * Fooyin
 * Copyright © 2023, Luke Taylor <LukeT1@proton.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "fyutils_export.h"

#include <QTabBar>

namespace Fooyin {
class PopupLineEdit;

class FYUTILS_EXPORT EditableTabBar : public QTabBar
{
    Q_OBJECT

public:
    explicit EditableTabBar(QWidget* parent = nullptr);

    [[nodiscard]] bool addButtonEnabled() const;
    void setAddButtonEnabled(bool enabled);

    void showEditor();
    void closeEditor();

signals:
    void addButtonClicked();
    void tabTextChanged(int index, const QString& text);

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool isAddButtonTab(const QPoint& pos);

    PopupLineEdit* m_lineEdit;
    bool m_showAddButton;
    bool m_addButtonAdded;
};
} // namespace Fooyin
