# ---------------------------------------------------------------------------
# Authenticode-signs CKFlip3D binaries as publisher "CYMERKAROL".
#
# Uses a self-signed code-signing certificate (CN=CYMERKAROL) from the
# current user's personal store; one is created automatically on first run
# (10-year validity).  Self-signed means Windows shows the publisher name in
# the file's digital-signature details, but SmartScreen/UAC still says
# "unknown publisher" unless the certificate is imported into the machine's
# Trusted Root / Trusted Publishers stores - swap in a commercial cert here
# whenever one is available, the build scripts won't need to change.
#
# Called by build_local.bat / build.bat, core\Settings\build_settings.bat
# and core\Installer\build_installer.bat after each successful build.
# Signing is best-effort: a failure prints a warning but never fails the
# build (e.g. on a machine without the cert store rights).
# ---------------------------------------------------------------------------
# Paths arrive as plain positional arguments (powershell -File does not
# parse "a","b" into an array) — collect every remaining argument.
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Path
)

$ErrorActionPreference = 'Stop'
$subject = 'CN=CYMERKAROL'

try {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1

    if (-not $cert) {
        Write-Host "sign: creating self-signed code-signing certificate $subject"
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
            -FriendlyName 'CKFlip3D code signing (CYMERKAROL)' `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyAlgorithm RSA -KeyLength 3072 `
            -NotAfter (Get-Date).AddYears(10)
    }
}
catch {
    Write-Warning "sign: could not obtain a signing certificate - $($_.Exception.Message)"
    exit 0
}

foreach ($file in $Path) {
    if (-not (Test-Path $file)) {
        Write-Warning "sign: skipped (not found): $file"
        continue
    }
    try {
        # Timestamp so the signature outlives the certificate; fall back to
        # an untimestamped signature when the timestamp server is offline.
        $sig = $null
        try {
            $sig = Set-AuthenticodeSignature -FilePath $file -Certificate $cert `
                -HashAlgorithm SHA256 -TimestampServer 'http://timestamp.digicert.com'
        }
        catch {
            $sig = Set-AuthenticodeSignature -FilePath $file -Certificate $cert `
                -HashAlgorithm SHA256
        }

        # Self-signed chains report UnknownError ("root not trusted") even
        # though the signature was applied - success == a signer is present.
        if ($sig -and $sig.SignerCertificate) {
            Write-Host "sign: OK ($($sig.Status)) $file"
        }
        else {
            Write-Warning "sign: FAILED ($($sig.Status)) $file"
        }
    }
    catch {
        Write-Warning "sign: FAILED $file - $($_.Exception.Message)"
    }
}
exit 0
