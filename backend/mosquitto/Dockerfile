# Custom Mosquitto image that bakes in the config + password file with the
# correct ownership/permissions. Mounting the passwd file from a Windows host
# gives it world-readable perms, which Mosquitto 2.x rejects ("Unable to open
# pwfile"). Copying it in at build time and chowning to the mosquitto user
# fixes it identically on Windows-dev and Linux-EC2.
FROM eclipse-mosquitto:2

COPY mosquitto.conf /mosquitto/config/mosquitto.conf
COPY passwd /mosquitto/config/passwd

# Mosquitto refuses a group/world-readable password file. Lock it down and give
# it to the mosquitto user the broker runs as.
USER root
RUN chown mosquitto:mosquitto /mosquitto/config/passwd \
 && chmod 0700 /mosquitto/config/passwd
USER mosquitto
