def parse_chip_local_core_rule(rule_value, cores_per_chip, where="chip rule"):
    """Parse a chip-local PCIe rule into explicit local core ids.

    Comma-separated form is always explicit, e.g. "0,4,5,9".
    Compact digit form is only accepted when every legal local core id is a
    single digit, e.g. "0459" for a 2x5 chip. Larger chips must use commas.
    """
    try:
        cores_per_chip = int(cores_per_chip)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{where} has invalid cores_per_chip {cores_per_chip!r}.") from exc
    if cores_per_chip <= 0:
        raise ValueError(f"{where} cores_per_chip must be positive.")

    rule_text = str(rule_value).strip().upper()
    if rule_text in ("", "NONE"):
        return []

    if "," in rule_text:
        tokens = [item.strip() for item in rule_text.split(",")]
        if any(item == "" for item in tokens):
            raise ValueError(f"{where} contains an empty local core token.")
    else:
        if not rule_text.isdigit():
            raise ValueError(f"{where} contains non-integer local core {rule_text!r}.")
        if len(rule_text) > 1:
            if cores_per_chip > 10:
                raise ValueError(
                    f"{where} compact rule {rule_text!r} is ambiguous for "
                    f"{cores_per_chip} local cores; use comma-separated ids."
                )
            if len(rule_text) == 2 and rule_text.startswith("0"):
                raise ValueError(
                    f"{where} compact rule {rule_text!r} is ambiguous; use "
                    "comma-separated ids for multiple cores or omit the leading zero."
                )
            tokens = list(rule_text)
        else:
            tokens = [rule_text]

    local_cores = []
    seen = set()
    for token in tokens:
        if not token.isdigit():
            raise ValueError(f"{where} contains non-integer local core {token!r}.")
        local_core = int(token)
        if local_core < 0 or local_core >= cores_per_chip:
            raise ValueError(
                f"{where} local core {local_core} is outside range 0~{cores_per_chip - 1}."
            )
        if local_core in seen:
            raise ValueError(f"{where} repeats local core {local_core}.")
        seen.add(local_core)
        local_cores.append(local_core)
    return local_cores


def format_chip_local_core_rule(local_cores):
    local_cores = list(local_cores)
    if not local_cores:
        return "NONE"
    return ",".join(str(local_core) for local_core in local_cores)
