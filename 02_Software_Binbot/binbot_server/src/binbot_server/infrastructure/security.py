from __future__ import annotations

import hmac
from collections.abc import Mapping

from binbot_server.domain.errors import AuthenticationFailed


class DeviceAuthenticator:
    def __init__(self, allow_anonymous: bool, tokens: Mapping[str, str]):
        self._allow_anonymous = allow_anonymous
        self._tokens = dict(tokens)

    def verify(self, device_id: str, supplied_token: str) -> None:
        expected = self._tokens.get(device_id)
        if expected is not None and hmac.compare_digest(expected, supplied_token):
            return
        if expected is None and self._allow_anonymous:
            return
        raise AuthenticationFailed("device credentials were rejected")
