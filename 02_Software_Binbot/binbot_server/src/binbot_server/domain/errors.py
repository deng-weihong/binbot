class BinBotError(Exception):
    """Base class for errors that can be translated into protocol responses."""

    code = "internal_error"


class ProtocolViolation(BinBotError):
    code = "protocol_violation"


class StateConflict(BinBotError):
    code = "state_conflict"


class AuthenticationFailed(BinBotError):
    code = "authentication_failed"


class ProviderFailure(BinBotError):
    code = "provider_failure"


class ProviderTimeout(ProviderFailure):
    code = "provider_timeout"
