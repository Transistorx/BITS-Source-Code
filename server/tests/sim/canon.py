"""Profile canonicalisation exactly as the relay firmware does it (CONTRACT 9.3 notes).

Members sorted by key (byte order), no whitespace outside strings, EVERY value token
copied verbatim from the received text (``1e-3`` stays ``1e-3``, ``0.10`` stays
``0.10``), then SHA-256, first 16 lowercase hex. Deliberately does NOT use json.loads
on the value tokens, so a server that hashes a re-serialised float is caught.
"""
import hashlib
import re

_WS = " \t\r\n"
_NUM = re.compile(r"-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?\Z")
_LIT = ("true", "false", "null")


class CanonError(ValueError):
    """.args[0] starts with the firmware ACK reason prefix (PIN_MALFORMED, PIN_DUPLICATE_KEY...)."""


def _skip(text: str, i: int) -> int:
    while i < len(text) and text[i] in _WS:
        i += 1
    return i


def _string(text: str, i: int) -> tuple[str, int]:
    if text[i] != '"':
        raise CanonError("PIN_MALFORMED expected string")
    j = i + 1
    while j < len(text):
        if text[j] == "\\":
            j += 2
            continue
        if text[j] == '"':
            return text[i:j + 1], j + 1
        j += 1
    raise CanonError("PIN_MALFORMED unterminated string")


def _value(text: str, i: int) -> tuple[str, int]:
    i = _skip(text, i)
    if i >= len(text):
        raise CanonError("PIN_MALFORMED truncated")
    ch = text[i]
    if ch == "{":
        return _object(text, i)
    if ch == "[":
        return _array(text, i)
    if ch == '"':
        return _string(text, i)
    j = i
    while j < len(text) and text[j] not in ",}] \t\r\n":
        j += 1
    token = text[i:j]
    if token not in _LIT and not _NUM.match(token):
        raise CanonError(f"PIN_MALFORMED bad token {token[:16]!r}")
    return token, j


def _array(text: str, i: int) -> tuple[str, int]:
    out, i = [], _skip(text, i + 1)
    if text[i:i + 1] == "]":
        return "[]", i + 1
    while True:
        tok, i = _value(text, i)
        out.append(tok)
        i = _skip(text, i)
        if text[i:i + 1] == ",":
            i += 1
            continue
        if text[i:i + 1] == "]":
            return "[" + ",".join(out) + "]", i + 1
        raise CanonError("PIN_MALFORMED array")


def _members(text: str, i: int) -> tuple[list[tuple[str, str]], int]:
    """-> ([(key_text_with_quotes, canonical_value)], index after '}')"""
    members: list[tuple[str, str]] = []
    i = _skip(text, i + 1)
    if text[i:i + 1] == "}":
        return members, i + 1
    while True:
        i = _skip(text, i)
        key, i = _string(text, i)
        i = _skip(text, i)
        if text[i:i + 1] != ":":
            raise CanonError("PIN_MALFORMED missing colon")
        val, i = _value(text, i + 1)
        if any(k == key for k, _ in members):
            raise CanonError(f"PIN_DUPLICATE_KEY {key.strip(chr(34))[:24]}")
        members.append((key, val))
        i = _skip(text, i)
        if text[i:i + 1] == ",":
            i += 1
            continue
        if text[i:i + 1] == "}":
            return members, i + 1
        raise CanonError("PIN_MALFORMED object")


def _object(text: str, i: int) -> tuple[str, int]:
    members, end = _members(text, i)
    members.sort(key=lambda kv: kv[0][1:-1].encode("utf-8"))
    return "{" + ",".join(f"{k}:{v}" for k, v in members) + "}", end


def canonical(obj_text: str) -> str:
    """Canonical text of one JSON object given as text."""
    text, end = _object(obj_text, _skip(obj_text, 0))
    if _skip(obj_text, end) != len(obj_text):
        raise CanonError("PIN_MALFORMED trailing data")
    return text


def top_level(text: str) -> dict[str, str]:
    """Top-level members of a command -> canonical value text. A duplicated top-level
    key is refused (the firmware refuses a second `profile` member)."""
    i = _skip(text, 0)
    if text[i:i + 1] != "{":
        raise CanonError("PIN_MALFORMED not an object")
    members, _ = _members(text, i)
    return {k[1:-1]: v for k, v in members}


def profile_hash_of(canonical_text: str) -> str:
    return hashlib.sha256(canonical_text.encode("utf-8")).hexdigest()[:16]


def hash_command_profile(command_text: str) -> tuple[str, str]:
    """(canonical profile text, hash) of the `profile` member of the received command text."""
    members = top_level(command_text)
    if "profile" not in members:
        raise CanonError("PIN_MALFORMED no profile")
    if not members["profile"].startswith("{"):
        raise CanonError("PIN_MALFORMED profile is not an object")
    return members["profile"], profile_hash_of(members["profile"])
