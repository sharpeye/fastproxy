/*
 * headers.hpp
 *
 *  Created on: Jul 8, 2010
 *      Author: nbryskin
 */

#ifndef HEADERS_HPP_
#define HEADERS_HPP_

#include <map>
#include <functional>
#include <cstring>
#include <string.h>

class lstring
{
public:
    lstring()
      : begin()
      , end()
    {
    }

    lstring(const char* cstr)
      : begin(cstr)
      , end(begin + std::strlen(cstr))
    {
    }

    lstring(const char* begin, const char* end)
      : begin(begin)
      , end(end)
    {
    }

    // Checks if header line is empty, i.e. has 0-length or contains just new line characters
    bool empty() const
    {
        switch (size())
        {
            case 0:     return true;
            case 1:     return ((begin[0] == '\r') || (begin[0] == '\n'));
            case 2:     return (begin[1] == '\n');
            default:    return false;
        }
    }

    std::size_t size() const
    {
        return end - begin;
    }

    friend bool operator < (const lstring& lhs, const lstring& rhs)
    {
        const char* l = lhs.begin;
        const char* r = rhs.begin;

        for (; l != lhs.end && r != rhs.end; l++, r++)
        {
            char lc = tolower(*l);
            char rc = tolower(*r);

            if (lc == rc)
                continue;

            return (lc < rc);
        }

        // It might be the case when 'allowed' header name is a prefix of another header
        // name. We don't want to allow headers by prefix but rather by exact match. So
        // the next character has to be checked and headers are equal if it is ':'.
        // The only case we have to check, in all other cases condition result is 'false'
        if (lhs.size() < rhs.size())
        {
            return rhs.begin[lhs.size()] != ':';
        }

        return false;
    }

    const char* begin;
    const char* end;
};

typedef std::map<lstring, lstring> headers_type;

#endif /* HEADERS_HPP_ */
