"""Transport-neutral service error.

Services raise ServiceError(code, message[, http_status]); the HTTP routes map it
to HTTPException(status_code=http_status, detail=message) (see routes/_adapt.py)
and a future MQTT dispatcher can map ``code`` to its own reply. No fastapi here.
"""

_DEFAULT_STATUS = {
    "invalid": 422,
    "not_found": 404,
    "conflict": 409,
    "unavailable": 503,
    "internal": 500,
    "no_compatible_profile": 422,
    "ambiguous_profile": 409,
    "target_out_of_profile_range": 422,
}


class ServiceError(Exception):
    def __init__(self, code: str, message: str, http_status: int | None = None,
                 data: dict | None = None):
        super().__init__(message)
        self.code = code
        self.message = message
        # Structured extras (e.g. AMBIGUOUS_PROFILE candidates[]); None for plain errors.
        self.data = data
        self.http_status = http_status if http_status is not None else _DEFAULT_STATUS.get(code, 500)

    @property
    def detail(self) -> str:  # same attribute name HTTPException exposes
        return self.message
