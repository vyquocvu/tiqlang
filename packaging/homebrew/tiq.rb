class Tiq < Formula
  desc "Tiny, deterministic compiled language for fast tools and services"
  homepage "https://github.com/vyquocvu/tiqlang"
  url "https://github.com/vyquocvu/tiqlang/archive/refs/tags/v0.1.0.tar.gz"
  sha256 "" # Fill in after release
  license "MIT"
  head "https://github.com/vyquocvu/tiqlang.git", branch: "main"

  depends_on "make" => :build

  def install
    system "make"
    bin.install "build/tiq"
    pkgshare.install "examples", "std", "docs"
  end

  test do
    (testpath/"hello.tiq").write <<~EOS
      print("Hello, Tiq!")
    EOS
    system bin/"tiq", "run", "hello.tiq"
  end
end
