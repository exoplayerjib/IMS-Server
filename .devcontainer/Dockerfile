# Start with the official Ubuntu 24.04 (Noble Numbat) base image
FROM ubuntu:24.04

# Prevent interactive prompts from blocking the build process
ENV DEBIAN_FRONTEND=noninteractive
ARG USERNAME=exodev
ARG USER_UID=1000
ARG USER_GID=1000

# Update the package index and install all necessary tools in a single layer
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Core C/C++ Development
    build-essential \
    gdb \
    cmake \
    #python
    python3 \
    python-is-python3 \
    # Java Development
    openjdk-25-jdk-headless \
    # Assembly & Computer Architecture 
    nasm \
    spim \
    # Databases & SQL Tools
    sqlite3 \
    postgresql-client \
    # Networking & Web Utilities
    curl \
    wget \
    net-tools \
    iproute2 \
    # Version Control
    git \
    gh \
    #bash alt
    fish \
    sudo \
    openssh-client \
    maven \
    # Clean up the apt cache to keep your final image size small
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*


RUN userdel -r ubuntu \
    && groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m -s /usr/bin/fish $USERNAME \
    && echo "$USERNAME ALL=(root) NOPASSWD:ALL" > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME

USER $USERNAME

# Set the default working directory inside the container
WORKDIR /workspace


# Start a bash shell by default when running the container manually
CMD ["/usr/bin/fish"]