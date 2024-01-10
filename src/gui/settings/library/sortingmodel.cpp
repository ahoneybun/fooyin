/*
 * Fooyin
 * Copyright 2022-2024, Luke Taylor <LukeT1@proton.me>
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

#include "sortingmodel.h"

#include "core/library/sortingregistry.h"

#include <utils/treestatusitem.h>

using namespace Qt::Literals::StringLiterals;

namespace Fooyin {
class SortingItem : public TreeStatusItem<SortingItem>
{
public:
    SortingItem()
        : SortingItem{{}, nullptr}
    { }

    explicit SortingItem(SortScript sortScript, SortingItem* parent)
        : TreeStatusItem{parent}
        , m_sortScript{std::move(sortScript)}
    { }

    [[nodiscard]] SortScript sortScript() const
    {
        return m_sortScript;
    }

    void changeSort(SortScript sortScript)
    {
        m_sortScript = std::move(sortScript);
    }

private:
    SortScript m_sortScript;
};

struct SortingModel::Private
{
    SortingRegistry* sortRegistry;
    SortingItem root;
    std::map<int, SortingItem> nodes;

    explicit Private(SortingRegistry* sortRegistry)
        : sortRegistry{sortRegistry}
    { }
};

SortingModel::SortingModel(SortingRegistry* sortRegistry, QObject* parent)
    : ExtendableTableModel{parent}
    , p{std::make_unique<Private>(sortRegistry)}
{ }

SortingModel::~SortingModel() = default;

void SortingModel::populate()
{
    beginResetModel();
    p->root = {};
    p->nodes.clear();

    const auto& sortScripts = p->sortRegistry->items();

    for(const auto& [index, sortScript] : sortScripts) {
        if(!sortScript.isValid()) {
            continue;
        }
        SortingItem* child = &p->nodes.emplace(index, SortingItem{sortScript, &p->root}).first->second;
        p->root.appendChild(child);
    }

    endResetModel();
}

void SortingModel::processQueue()
{
    std::vector<SortingItem> sortScriptsToRemove;

    for(auto& [index, node] : p->nodes) {
        const SortingItem::ItemStatus status = node.status();
        const SortScript sortScript          = node.sortScript();

        switch(status) {
            case(SortingItem::Added): {
                if(sortScript.script.isEmpty()) {
                    break;
                }

                const auto addedSort = p->sortRegistry->addItem(sortScript);
                if(addedSort.isValid()) {
                    node.changeSort(addedSort);
                    node.setStatus(SortingItem::None);

                    emit dataChanged({}, {}, {Qt::DisplayRole, Qt::FontRole});
                }
                else {
                    qWarning() << "Sorting " + sortScript.name + " could not be added";
                }
                break;
            }
            case(SortingItem::Removed): {
                if(p->sortRegistry->removeByIndex(sortScript.index)) {
                    beginRemoveRows({}, node.row(), node.row());
                    p->root.removeChild(node.row());
                    endRemoveRows();
                    sortScriptsToRemove.push_back(node);
                }
                else {
                    qWarning() << "Sorting " + sortScript.name + " could not be removed";
                }
                break;
            }
            case(SortingItem::Changed): {
                if(p->sortRegistry->changeItem(sortScript)) {
                    node.changeSort(p->sortRegistry->itemById(sortScript.id));
                    node.setStatus(SortingItem::None);

                    emit dataChanged({}, {}, {Qt::DisplayRole, Qt::FontRole});
                }
                else {
                    qWarning() << "Sorting " + sortScript.name + " could not be changed";
                }
                break;
            }
            case(SortingItem::None):
                break;
        }
    }
    for(const auto& item : sortScriptsToRemove) {
        p->nodes.erase(item.sortScript().index);
    }
}

Qt::ItemFlags SortingModel::flags(const QModelIndex& index) const
{
    if(!index.isValid()) {
        return Qt::NoItemFlags;
    }

    auto flags = ExtendableTableModel::flags(index);
    flags |= Qt::ItemIsEditable;

    return flags;
}

QVariant SortingModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(role == Qt::TextAlignmentRole) {
        return (Qt::AlignHCenter);
    }

    if(role != Qt::DisplayRole || orientation == Qt::Orientation::Vertical) {
        return {};
    }

    switch(section) {
        case(0):
            return "Index";
        case(1):
            return "Name";
        case(2):
            return "Sort Script";
    }
    return {};
}

QVariant SortingModel::data(const QModelIndex& index, int role) const
{
    if(role != Qt::DisplayRole && role != Qt::EditRole && role != Qt::FontRole && role != Qt::UserRole) {
        return {};
    }

    if(!checkIndex(index, CheckIndexOption::IndexIsValid)) {
        return {};
    }

    auto* item = static_cast<SortingItem*>(index.internalPointer());

    if(role == Qt::FontRole) {
        return item->font();
    }

    if(role == Qt::UserRole) {
        return QVariant::fromValue(item->sortScript());
    }

    switch(index.column()) {
        case(0):
            return item->sortScript().index;
        case(1): {
            const QString& name = item->sortScript().name;
            return !name.isEmpty() ? name : QStringLiteral("<enter name here>");
        }
        case(2): {
            const QString& field = item->sortScript().script;
            return !field.isEmpty() ? field : QStringLiteral("<enter sort script here>");
        }
    }

    return {};
}

bool SortingModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if(role != Qt::EditRole) {
        return false;
    }

    auto* item            = static_cast<SortingItem*>(index.internalPointer());
    SortScript sortScript = item->sortScript();

    switch(index.column()) {
        case(1): {
            if(value.toString() == "<enter name here>"_L1 || sortScript.name == value.toString()) {
                if(item->status() == SortingItem::Added) {
                    emit pendingRowCancelled();
                }
                return false;
            }
            sortScript.name = value.toString();

            emit pendingRowAdded();
            break;
        }
        case(2): {
            if(sortScript.script == value.toString()) {
                return false;
            }
            sortScript.script = value.toString();
            break;
        }
        case(0):
            break;
    }

    if(item->status() == SortingItem::None) {
        item->setStatus(SortingItem::Changed);
    }

    item->changeSort(sortScript);
    emit dataChanged({}, {}, {Qt::DisplayRole, Qt::FontRole});

    return true;
}

QModelIndex SortingModel::index(int row, int column, const QModelIndex& parent) const
{
    if(!hasIndex(row, column, parent)) {
        return {};
    }

    SortingItem* item = p->root.child(row);

    return createIndex(row, column, item);
}

int SortingModel::rowCount(const QModelIndex& /*parent*/) const
{
    return p->root.childCount();
}

int SortingModel::columnCount(const QModelIndex& /*parent*/) const
{
    return 3;
}

bool SortingModel::removeRows(int row, int count, const QModelIndex& /*parent*/)
{
    for(int i{row}; i < row + count; ++i) {
        const QModelIndex& index = this->index(i, 0, {});

        if(!index.isValid()) {
            return false;
        }

        auto* item = static_cast<SortingItem*>(index.internalPointer());
        if(item) {
            if(item->status() == SortingItem::Added) {
                beginRemoveRows({}, i, i);
                p->root.removeChild(i);
                endRemoveRows();
                p->nodes.erase(item->sortScript().index);
            }
            else {
                item->setStatus(SortingItem::Removed);
                emit dataChanged({}, {}, {Qt::FontRole});
            }
        }
    }
    return true;
}

void SortingModel::addPendingRow()
{
    const int index = static_cast<int>(p->nodes.size());

    SortScript sortScript;
    sortScript.index = index;

    SortingItem* item = &p->nodes.emplace(index, SortingItem{sortScript, &p->root}).first->second;

    item->setStatus(SortingItem::Added);

    const int row = p->root.childCount();
    beginInsertRows({}, row, row);
    p->root.appendChild(item);
    endInsertRows();

    emit newPendingRow();
}

void SortingModel::removePendingRow()
{
    const int row = rowCount({}) - 1;
    beginRemoveRows({}, row, row);
    p->root.removeChild(row);
    endRemoveRows();
}
} // namespace Fooyin
