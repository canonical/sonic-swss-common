#pragma once

#include <iostream>
#include <ios>

namespace swss
{
class Boolean
{
public:
    Boolean() = default;
    Boolean(bool boolean) : m_boolean(boolean)
    {
    }
    operator bool() const
    {
        return m_boolean;
    }
protected:
    bool m_boolean;
};

class AlphaBoolean : public Boolean
{
public:
    AlphaBoolean() = default;
    AlphaBoolean(bool boolean) : Boolean(boolean)
    {
    }
};

static inline std::ostream &operator<<(std::ostream &out, const AlphaBoolean &b)
{
    return out << std::boolalpha << (bool)(b);
}

static inline std::istream &operator>>(std::istream &in, AlphaBoolean &b)
{
    bool tmp;
    in >> std::boolalpha >> tmp;
    if (in)
    {
        b = tmp;
    }
    return in;
}

static inline std::ostream &operator<<(std::ostream &out, const Boolean &b)
{
    return out << (bool)(b);
}

static inline std::istream &operator>>(std::istream &in, Boolean &b)
{
    bool tmp;
    in >> tmp;
    if (in)
    {
        b = tmp;
    }
    return in;
}

}
