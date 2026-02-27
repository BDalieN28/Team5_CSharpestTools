# Team5_CSharpestTools
Repository for my team's chess engine.
```mermaid
sequenceDiagram
	autonumber
	actor Host as Host / GUI (Arena)
	participant Engine as Engine(Main loop)
	participant Parser as UCI Parser
	participant Position as Position (Board/FEN)
	participant MoveGen as MoveGenerator (pseudoLegalMoves etc.)
	participant Rule as RuleChecker (isSquareAttacked / inCheck)
	participant MoveObj as Move (Move.fromUci / toUci)

	%%Note over Host,Engine: Engine runs a UCI stdin/stdout loop

	Host ->> Engine: "uci"
	activate Engine
	Engine ->> Engine: parse "uci"
	Engine -->> Host: "id name <engine>"
	Engine -->> Host: "id author <author>"
	Engine -->> Host: "uciok"
	deactivate Engine

	Host ->> Engine: "isready"
	activate Engine
	Engine ->> Engine: parse "isready"
	Engine -->> Host: "readyok"
	deactivate Engine

	Host ->> Engine: "ucinewgame"
	activate Engine
	Engine ->> Position: Position.startPos() // reset position to starting
	Engine -->> Host: (ack no response required)
	deactivate Engine

	Host ->> Engine: "position startpos moves e2e4 e7e5..."
	activate Engine
	Engine ->> Parser: parse "position..." (detect startpos (or fen))
	Parser ->> Position: create Position (startPos (or fromFEN))
	loop for each move taken
		Parser ->> MoveObj: Move.fromUci("e2e4")
		MoveObj -->> Parser: Move Object
		Parser ->> Position: Position = Position.makeMove(Move)
	end
	Engine -->> Host: (no response required)
	deactivate Engine

	Host ->> Engine: "go movetime 10000"
	activate Engine
	Engine ->> Parser: parse "go" options (movetime/wtime/etc.)
	Engine ->> MoveGen: legalMoves = Position.legalMoves()
	activate MoveGen
	MoveGen -->> Engine: returns legalMoves list
	deactivate MoveGen

	Engine ->> Engine: choose first move (legalMoves.get(0))
	Engine ->> MoveObj: selectedMove.toUci()
	Engine -->> Host: "bestmove e2e4"
	deactivate Engine


	Host ->> Engine: "quit"
	activate Engine
	Engine ->> Engine: cleanup and exit
	Engine -->> Host: (process ends)
	deactivate Engine
```
