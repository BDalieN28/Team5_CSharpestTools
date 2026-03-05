# Team5_CSharpestTools
Repository for my team's chess engine.  
Members: Brandon Deel, Nathaniel Garcia, Daniel Kim, Zachary Agle, Kevin Wang, Kevin Yu!
//Pull Request change example comment
//Pull request created 3/2/26 at approximately 11pm

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
