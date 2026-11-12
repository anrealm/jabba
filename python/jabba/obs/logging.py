import sys

from loguru import logger

FORMAT = (
    "<green>{time:YYYY-MM-DD HH:mm:ss.SSS}</green> | <level>{level: <8}</level> | "
    "req={extra[request_id]} | <cyan>{name}</cyan>:<cyan>{line}</cyan> - {message}"
)


def configure_logging(level: str = "INFO") -> None:
    """Technical log: for humans and grep. One line per event, request_id everywhere."""
    logger.remove()
    logger.configure(extra={"request_id": "-"})
    logger.add(sys.stderr, level=level, format=FORMAT)
