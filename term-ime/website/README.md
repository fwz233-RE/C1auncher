# Website

This website is built using [Docusaurus](https://docusaurus.io/), a modern static website generator.

## Installation

```bash
npm install
```

**Note**: feel free to use the package manager of your choice.

## Local Development

```bash
npm run start
```

This command starts a local development server and opens up a browser window. Most changes are reflected live without having to restart the server.

## Build

```bash
npm run build
```

This command generates static content into the `build` directory and can be served using any static contents hosting service.

## Deployment

The website sources are maintained in `term-ime/website/` inside the C1auncher repository. This source import does not publish a website or enable GitHub Pages. The configuration targets the new repository for a future deployment; the former upstream installation script is not distributed here.

Deploy only after the repository maintainers explicitly configure and approve the hosting location. Local development and `npm run build` work independently of Pages.
