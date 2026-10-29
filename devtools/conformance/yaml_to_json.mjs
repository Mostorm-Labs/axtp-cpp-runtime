import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
const require=createRequire(new URL("../generators/package.json",import.meta.url));
const YAML=require("yaml");
process.stdout.write(JSON.stringify(YAML.parse(readFileSync(process.argv[2],"utf8"))));
