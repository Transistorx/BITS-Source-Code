"""Thin adapter: service-layer ServiceError -> the HTTPException the API always raised."""

from contextlib import contextmanager

from fastapi import HTTPException

from ..services.errors import ServiceError


@contextmanager
def http_errors():
    try:
        yield
    except ServiceError as exc:
        detail = ({"code": exc.code.upper(), "message": exc.message, **exc.data}
                  if exc.data else exc.message)
        raise HTTPException(status_code=exc.http_status, detail=detail) from exc
