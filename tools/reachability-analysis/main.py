#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import sys

from lib.core import ReachabilityGraph

PROMPT_MSG = "Enter a node's name or 's {search term}' to search for one: "


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="""A tool to query a graph generated by Redex's Reachability service.
It only supports ReachabilityGraph for now (as opposed to MethodOverrideGraph
or CombinedGraph) but feel free to extend it if needed.

Once a graph is loaded, the interface supports 3 operations:
- 's Foo' to search and print all nodes that contain 'Foo'
- Providing a node name (e.g., 'Landroid/app/IntentService;') will show
  the node, how it's reached and what it references in the graph
- 'x' to exit""",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument(
        "-i",
        "--input",
        help="Input file generated by Reachability.dump_graph()",
        required=True,
    )

    return parser.parse_args(argv)


def get_user_input():
    return input(PROMPT_MSG)


def main(argv):
    args = parse_args(argv)
    graph = ReachabilityGraph()
    print("Loading graph. This might take a couple of minutes")
    graph.load(args.input)

    input_str = input(PROMPT_MSG)
    while input_str != "x":
        if not input_str.strip():
            input_str = input(PROMPT_MSG)
            continue
        if input_str.startswith("s "):
            graph.list_nodes(input_str[2:])
            input_str = input(PROMPT_MSG)
            continue

        # Search for the node and print it
        try:
            node = graph.get_node(input_str)
            print(repr(node))
        except KeyError:
            print(f"Cannot find a node named '{input_str}'")
        input_str = input(PROMPT_MSG)


if __name__ == "__main__":
    main(sys.argv[1:])
