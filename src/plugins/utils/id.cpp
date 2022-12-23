#include "id.h"

namespace {
unsigned int idFromString(const QString& str)
{
    unsigned int result{0};
    if(!str.isEmpty()) {
        result = std::hash<QString>{}(str);
    }
    return result;
}
} // namespace

namespace Util {
Id::Id(const QString& str)
    : m_id{idFromString(str)}
    , m_name{str}
{ }

Id::Id(const char* const str)
    : m_id{idFromString(str)}
    , m_name{str}
{ }

bool Id::isValid() const
{
    return (m_id > 0 && !m_name.isNull());
}

unsigned int Id::id() const
{
    return m_id;
}

QString Id::name() const
{
    return m_name;
}

Id Id::append(const QString& str)
{
    QString name = m_name.append(str);
    return Id{name};
}

Id Id::append(const char* const str)
{
    return append(QString{str});
}

Id Id::append(int num)
{
    QString name = m_name.append(QString::number(num));
    return Id{name};
}

bool Id::operator==(const Id& id) const
{
    return m_id == id.m_id;
}

bool Id::operator!=(const Id& id) const
{
    return m_id != id.m_id;
}

size_t qHash(const Id& id) noexcept
{
    return static_cast<size_t>(id.m_id);
}
} // namespace Util
