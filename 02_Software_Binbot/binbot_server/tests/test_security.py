import pytest

from binbot_server.domain.errors import AuthenticationFailed
from binbot_server.infrastructure.security import DeviceAuthenticator


def test_known_device_requires_matching_token() -> None:
    authenticator = DeviceAuthenticator(True, {"board-01": "secret-token"})
    authenticator.verify("board-01", "secret-token")
    with pytest.raises(AuthenticationFailed):
        authenticator.verify("board-01", "wrong-token")


def test_unknown_device_obeys_anonymous_policy() -> None:
    DeviceAuthenticator(True, {}).verify("development-board", "")
    with pytest.raises(AuthenticationFailed):
        DeviceAuthenticator(False, {}).verify("unknown-board", "")
