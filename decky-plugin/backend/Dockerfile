# Used by the Decky CLI (`decky plugin build`): builds bin/ds5bridged inside a
# SteamOS (holo) userland so the binary matches the Deck's glibc.
FROM ghcr.io/steamdeckhomebrew/holo-base:latest
RUN pacman -Sy --noconfirm --needed cmake git make gcc || true
ENTRYPOINT [ "/backend/entrypoint.sh" ]
