#include "core/units/Units.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace hocket::units
{
namespace
{
    std::string quoted (std::string_view text)
    {
        return "'" + std::string (text) + "'";
    }

    bool isDigit (char c)
    {
        return c >= '0' && c <= '9';
    }

    bool equalIgnoringCase (std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower (static_cast<unsigned char> (a[i])) != std::tolower (static_cast<unsigned char> (b[i])))
                return false;

        return true;
    }

    std::string unitList (const Quantity& quantity)
    {
        std::string list;

        for (std::size_t i = 0; i < quantity.units.size(); ++i)
            list += (i == 0 ? "" : i + 1 == quantity.units.size() ? " or " : ", ") + std::string (quantity.units[i].symbol);

        return list;
    }

    double roundTo (double value, int decimals)
    {
        const auto factor = std::pow (10.0, decimals);
        const auto rounded = std::round (value * factor) / factor;
        return rounded == 0.0 ? 0.0 : rounded; // never -0
    }

    constexpr Unit decibels[] = { { "dB", 1.0 } };
}

ParsedValue parse (std::string_view text, const Quantity& quantity)
{
    const auto name = std::string (quantity.name);

    const auto fail = [&] (const std::string& why)
    {
        return ParsedValue { {}, quoted (text) + " isn't a valid " + name + ": " + why + ". Write a number and a unit, e.g. "
                                     + std::string (quantity.examples) };
    };

    std::size_t i = 0;
    const bool negative = i < text.size() && text[i] == '-';

    if (i < text.size() && (text[i] == '-' || text[i] == '+'))
        ++i;

    const bool infinite = text.substr (i).starts_with ("inf");
    double number = 0.0;

    if (infinite)
    {
        i += 3;
    }
    else
    {
        const auto start = i;

        while (i < text.size() && isDigit (text[i]))
            ++i;

        if (i == start)
            return fail (text.empty() ? std::string ("it's empty") : std::string ("it needs a number first"));

        if (i < text.size() && text[i] == '.')
        {
            const auto point = i++;

            while (i < text.size() && isDigit (text[i]))
                ++i;

            if (i == point + 1)
                return fail ("a '.' needs digits after it");
        }

        // Locale-independent, unlike strtod.
        if (const auto [end, error] = std::from_chars (text.data() + start, text.data() + i, number); error != std::errc())
            return fail ("the number is too large");
    }

    while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
        ++i;

    const auto symbol = text.substr (i);
    const Unit* unit = nullptr;

    for (const auto& candidate : quantity.units)
        if (candidate.symbol == symbol)
            unit = &candidate;

    if (unit == nullptr)
    {
        if (symbol.empty())
            return fail ("it has no unit; " + name + " is written in " + unitList (quantity));

        for (const auto& candidate : quantity.units)
            if (equalIgnoringCase (candidate.symbol, symbol))
                return fail ("units are case-sensitive: write " + quoted (candidate.symbol) + ", not " + quoted (symbol));

        return fail (quoted (symbol) + " isn't a unit " + name + " is written in; use " + unitList (quantity));
    }

    if (infinite)
    {
        if (! quantity.minusInfinity)
            return fail ("it needs a number, not 'inf'");

        if (! negative)
            return fail ("only '-inf' is allowed, with the minus sign");

        return { -std::numeric_limits<double>::infinity(), {} };
    }

    const auto value = roundTo ((negative ? -number : number) * unit->scale, quantity.decimals);

    if (value < quantity.min || value > quantity.max)
        return { {}, quoted (text) + " is out of range: " + name + " goes from " + format (quantity.min, quantity) + " to "
                         + format (quantity.max, quantity) + (quantity.minusInfinity ? ", or -inf" + std::string (quantity.units.front().symbol) : "")
                         + ", e.g. " + std::string (quantity.examples) };

    return { value, {} };
}

std::string format (double value, const Quantity& quantity)
{
    const auto unit = std::string (quantity.units.front().symbol);

    if (std::isinf (value))
        return "-inf" + unit;

    char buffer[64];
    const auto [end, error] = std::to_chars (buffer, buffer + sizeof (buffer), value, std::chars_format::fixed, quantity.decimals);
    std::string number (buffer, error == std::errc() ? end : buffer);

    if (number.find ('.') != std::string::npos)
    {
        while (number.back() == '0')
            number.pop_back();

        if (number.back() == '.')
            number.pop_back();
    }

    if (number == "-0")
        number = "0";

    if (quantity.plusSign && value > 0.0)
        number = "+" + number;

    return number + unit;
}

const Quantity& volume()
{
    static const Quantity quantity { "volume", decibels, -96.0, 6.0, 1, true, true, "'-6dB', '0dB' (unchanged), '+3dB', '-infdB' (silence)" };
    return quantity;
}

//==============================================================================
ParsedPan parsePan (std::string_view text)
{
    const auto fail = [text] (const std::string& why)
    {
        return ParsedPan { {}, quoted (text) + " isn't a pan position: " + why
                                   + ". Pan is 'C' (centre), 'L1' to 'L100' (left) or 'R1' to 'R100' (right), e.g. 'L20'" };
    };

    if (text == "C")
        return { 0, {} };

    if (text.empty())
        return fail ("it's empty");

    const auto side = text[0];

    if (side == 'c' || side == 'l' || side == 'r')
    {
        auto upper = std::string (text);
        upper[0] = static_cast<char> (std::toupper (static_cast<unsigned char> (side)));

        if (parsePan (upper).value)
            return fail ("write it in capitals: " + quoted (upper));
    }

    if (side == 'C')
        return fail ("the centre is just 'C'");

    if (side != 'L' && side != 'R')
        return fail ("it must be C, or a capital L or R and a number");

    const auto digits = text.substr (1);
    int amount = 0;

    if (digits.empty() || digits.size() > 3 || ! std::ranges::all_of (digits, isDigit))
        return fail (std::string (1, side) + " needs a whole number from 1 to 100 after it");

    std::from_chars (digits.data(), digits.data() + digits.size(), amount);

    if (amount == 0)
        return fail ("the centre is written 'C'");

    if (amount > maxPan)
        return fail (side == 'L' ? "hard left is L100" : "hard right is R100");

    return { side == 'L' ? -amount : amount, {} };
}

std::string formatPan (int pan)
{
    if (pan == 0)
        return "C";

    return (pan < 0 ? "L" : "R") + std::to_string (pan < 0 ? -pan : pan);
}
}
