# Installing Tiq with Homebrew

## Installation

Once a release is published, install Tiq with:

```sh
brew tap vyquocvu/tiq
brew install tiq
```

## Development Version

Install the latest development version from the main branch:

```sh
brew install --head tiq
```

## Manual Installation

If you prefer to build from source without Homebrew:

```sh
git clone https://github.com/vyquocvu/tiqlang.git
cd tiqlang
make
sudo cp build/tiq /usr/local/bin/
```

## Verification

After installation, verify Tiq is working:

```sh
tiq --version
tiq run examples/hello.tiq
```

## Uninstallation

```sh
brew uninstall tiq
brew untap vyquocvu/tiq
```

## Troubleshooting

### Build fails with "make: command not found"

Install Xcode Command Line Tools:

```sh
xcode-select --install
```

### Permission denied when running tiq

Ensure the binary is executable:

```sh
chmod +x $(which tiq)
```

### Examples not found

Examples are installed to `$(brew --prefix tiq)/share/tiq/examples/`. Copy them to your working directory:

```sh
cp -r $(brew --prefix tiq)/share/tiq/examples ./
```
