#!/usr/bin/env bash
# Configure a stable Android signing identity without committing private keys.
set -euo pipefail
umask 077

REPO="${RADIO_FLEET_GITHUB_REPO:-mharlos/MeshCore}"
HOME_DIR="${RADIO_FLEET_SIGNING_DIR:-$HOME/.config/farm-radio-fleet}"
KEYSTORE="$HOME_DIR/release-signing.p12"
PASSFILE="$HOME_DIR/release-signing-password"
ALIAS="farmradiofleet"

for tool in gh keytool openssl base64 tr; do
  command -v "$tool" >/dev/null || {
    printf 'Missing %s. Install before continuing.\n' "$tool" >&2
    exit 1
  }
done
gh auth status >/dev/null
mkdir -p "$HOME_DIR"
chmod 700 "$HOME_DIR"

if [[ -f "$KEYSTORE" && -f "$PASSFILE" ]]; then
  printf 'Reusing existing signing identity from %s\n' "$KEYSTORE"
elif [[ ! -e "$KEYSTORE" && ! -e "$PASSFILE" ]]; then
  printf 'Generating a persistent, 3072-bit RSA signing certificate...\n'
  SECRET="$(openssl rand -hex 32)"
  export RADIO_FLEET_LOCAL_PASSWORD="$SECRET"
  keytool -genkeypair \
    -alias "$ALIAS" \
    -keystore "$KEYSTORE" \
    -storetype PKCS12 \
    -keyalg RSA -keysize 3072 \
    -sigalg SHA256withRSA \
    -validity 10950 \
    -dname "CN=Farm Radio Fleet, OU=Android, O=Flock and Furrow" \
    -storepass:env RADIO_FLEET_LOCAL_PASSWORD \
    -keypass:env RADIO_FLEET_LOCAL_PASSWORD \
    -noprompt >/dev/null
  printf '%s' "$SECRET" > "$PASSFILE"
  chmod 600 "$PASSFILE" "$KEYSTORE"
  unset SECRET RADIO_FLEET_LOCAL_PASSWORD
  printf 'Created new signing key.\n'
else
  printf '%s\n' 'Incomplete signing identity. Restore BOTH release-signing.p12 and release-signing-password from backup.' >&2
  exit 1
fi

export RADIO_FLEET_LOCAL_PASSWORD="$(cat "$PASSFILE")"
keytool -list \
  -keystore "$KEYSTORE" -storepass:env RADIO_FLEET_LOCAL_PASSWORD \
  -alias "$ALIAS" >/dev/null

printf 'Uploading persistent signing certificate to GitHub Actions Secrets in %s...\n' "$REPO"
base64 < "$KEYSTORE" | tr -d '\n' |
  gh secret set RADIO_FLEET_KEYSTORE_B64 --repo "$REPO"
printf '%s' "$RADIO_FLEET_LOCAL_PASSWORD" |
  gh secret set RADIO_FLEET_SIGNING_PASSWORD --repo "$REPO"

printf '\nSigning certificate details:\n'
keytool -list -v \
  -keystore "$KEYSTORE" -storepass:env RADIO_FLEET_LOCAL_PASSWORD \
  -alias "$ALIAS" | grep -E 'Alias name:|SHA256:|Valid from:' || true
unset RADIO_FLEET_LOCAL_PASSWORD

cat <<EOF

Signing setup complete.
Key: $KEYSTORE
Password recovery file: $PASSFILE

Back up BOTH files securely. Losing the key breaks the Android upgrade chain.
Never commit either file to your public GitHub repository.

Now trigger a signed APK build:
  gh workflow run build-radio-fleet-apk.yml --repo "$REPO" --ref feature/radio-fleet-sync

After the new signed APK is available, uninstall the old debug-signed app one
final time. All future builds signed by this certificate upgrade in place.
EOF
